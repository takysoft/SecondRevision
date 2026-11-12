// SecondRevision - the level's fine bevel (docs/terv-latvany-roadmap.md 9.3): the hard outward edges of the level's
// walls as the rounding (LevelRound.h) draws them rounded by a ball of a small radius, the way EdgeBevel.h rounds them:
// the faces cut along the edges' bands, the bands' points moved onto the round, and the polygons drawn from then on as
// the rounding's moved ones are.
//
// - A face (FBevelPrep) is a flat convex piece of a wall where it is drawn: a polygon the rounding left where it was, by
//   its ring (FResult's rings, the T-junctions in them), or one triangle of a polygon the rounding moved, where the
//   rounding put it - a wall beside a curve the rounding made, or one whose outline follows one, has its edges rounded
//   as a still one has.
// - A hard side is a side of a face where two faces of different planes meet sharper than the rounding's angle - the
//   flatter ones are the rounding's own -, outward (a box's) or inward (where a wall rises from a floor).
// - An edge to bevel (RdxBevel::FEdge) is a straight run of outward hard sides between two faces the bevel may cut -
//   walls, no sheet drawn from both sides, none left to the engine -, flatter than a knife's: the sides that touch one
//   after another along the line of the same two planes, from the first one's start to the last one's end. Along a
//   curve the rounding made, each side its triangles have there is a short edge of its own, and the next one's band
//   goes on where its own ends.
// - An edge of a polygon the relief may be drawn on is bevelled as any other: the band's triangles are the relief's
//   floor then (LevelRound.h's PackRound), and its patches lie on the round as they lie on the rounding's curves.
// - Left sharp, as scripts/bevel_survey.py counts them:
//   - an edge with a free end: no other hard side ends or passes within Near of it - the faces go on past the end, or
//     the line goes on as a hard side the bevel does not round -, where the band would stop in a step;
//   - a thin edge: another outward hard side on one of its planes, along it and beside it, nearer than twice the radius
//     - the face is too narrow for both bands, and the worn solid is empty there.
//
// Engine-independent, as LevelRound.h: the level check runs the same code.
#pragma once

#include "EdgeBevel.h"
#include "LevelRound.h"

namespace RdxLevelRound
{

// What the bevel found: the outward hard sides between faces it may cut, the edges they make up, and of those the ones
// left sharp, by why.
struct FBevelStats
{
    int Sides = 0, Edges = 0;
    int LeftFree = 0, LeftThin = 0;
};

namespace BevelDetail
{

static const double SameWay      = 0.99999;   // two ways this near (the cosine) are one
static const double Parallel     = 0.999;     // two hard sides this near the same way lie side by side
// An edge turning this far or further is a knife's, and left sharp: a wedge sharper than thirty degrees would wear back
// by more than three radii - the ball would take the blade off, not round its edge.
static const double KnifeDegrees = 150.0;
static const double Near         = 0.5;       // units: a hard side this near an edge's end ends or passes at it
static const double OnPlane      = 0.01;      // units: a place this near a plane lies on it
static const double SideCell     = 16.0;      // units: the cells the hard sides are found by
static const double ReachCell    = 64.0;      // units: the cells the hard sides' reach is found by, a triangle's box
static const double SliverShare  = 1e-3;      // a triangle no higher than this share of its longest side has no plane
#ifdef LEVELBEVEL_TRACE
// The level check's look at one place's move (LRC_BEVEL_TRACE): the faces on it and which of them keep it in their plane.
inline int& LevelBevelTracePlace()
{
    static int W = -1;
    return W;
}
// How many places the faces kept from most of their round's move (over three quarters of it lost, the round's move over
// a tenth of a unit), and of them how many stayed whole (three planes or more).
inline long long* LevelBevelKeptCounts()
{
    static long long Count[2] = { 0, 0 };
    return Count;
}
// Where every place lay before the bevel's own move - the rounding's where it drew them, a new one where the cut put
// it - for the level check's look at what the bevel turned over.
inline RdxSphere::TList<RdxSphere::FV3>& LevelBevelTraceBefore()
{
    static RdxSphere::TList<RdxSphere::FV3> Before;
    return Before;
}
// And how many of them the rounding had: the bevel's own come after.
inline int& LevelBevelTracePlaces0()
{
    static int Count = 0;
    return Count;
}
// The level check's A/B of the curved walls' facets the bands cut too (LRC_BEVEL_FACETS_OFF): true, only the faces in
// an edge's face's plane are cut, and a long facet holds a corner's move, as before.
inline bool& LevelBevelFacetsOff()
{
    static bool bOff = false;
    return bOff;
}
#endif
// Radii: how far a face no band cuts may reach from a place that moves and still tilt with it - the round's own reach,
// a point moving by at most three of them (EdgeBevel.h's MostWear), and one more.
static const double TiltReach    = 4.0;
static const double TCell       = 4.0;       // units: the cells the places on the faces' sides are found by
static const int    PlaceBits = 21, FaceBits = 21;   // a face side's key: its two places and its face, in 63 bits

// A hard side: its places, from U to V as polygon A goes round, the polygon across it (B), whether the edge there is
// outward, and whether it is one to bevel (both polygons still, flatter than a knife's).
struct FSide
{
    int  U, V, A, B;
    bool bOut, bBevel;
};

// The first of side s's run (a union-find, the way halved on each look).
inline int RunOf(TList<int>& Up, int s)
{
    while (Up[s] != s)
    {
        Up[s] = Up[Up[s]];
        s     = Up[s];
    }
    return s;
}

inline FV3 NormalOf(const FPolyIn& P) { return RdxSphere::V3(P.Nx, P.Ny, P.Nz); }

// The middle of polygon p's ring: a point of it off every side, the polygon being convex.
inline FV3 RingMiddle(const FResult& R, int p)
{
    FV3       M = RdxSphere::V3(0.0, 0.0, 0.0);
    const int F = R.RingFirst[p], C = R.RingCount[p];
    for (int i = 0; i < C; i++)
        M = M + R.Orig[R.Ring[F + i]] * (1.0 / C);
    return M;
}

// Whether polygon p's ring comes to one place twice - corners of the level's welded together into a ring that turns
// back on itself, which no cut along a line goes through whole.
inline bool RingTouchesItself(const FResult& R, int p)
{
    const int F = R.RingFirst[p], C = R.RingCount[p];
    for (int i = 0; i < C; i++)
        for (int j = i + 1; j < C; j++)
            if (R.Ring[F + i] == R.Ring[F + j])
                return true;
    return false;
}

static const double OffPlane = 0.05;   // units: a ring's place this far off its polygon's plane leaves it uncut

// Whether polygon p's ring lies in its plane (within OffPlane) and turns one way round it all along - a corner on the
// line of its neighbours (within OnEdge) turns neither way. The bevel's cuts along lines keep a flat convex ring's
// pieces flat and convex; a ring off its plane, or turning back, the level has a few of, and they are left uncut.
inline bool RingFlatAndConvex(const FResult& R, const FPolyIn& P, int p)
{
    const int F = R.RingFirst[p], C = R.RingCount[p];
    const FV3 N = NormalOf(P);
    const double D = RdxSphere::Dot(N, R.Orig[R.Ring[F]]);
    int Way = 0;
    for (int i = 0; i < C; i++)
    {
        const FV3& A = R.Orig[R.Ring[F + (i + C - 1) % C]];
        const FV3& B = R.Orig[R.Ring[F + i]];
        const FV3& Cc = R.Orig[R.Ring[F + (i + 1) % C]];
        if (fabs(RdxSphere::Dot(N, B) - D) > OffPlane)
            return false;
        // A corner within OnEdge of its neighbours' line lies on it: a T-junction's place may lie that far off its
        // side.
        const double Turn = RdxSphere::Dot(RdxSphere::Cross(B - A, Cc - B), N);
        if (fabs(Turn) <= OnEdge * RdxSphere::Length(Cc - A))
            continue;
        const int Here = (Turn > 0.0) ? 1 : -1;
        if (Way != 0 && Here != Way)
            return false;
        Way = Here;
    }
    return Way != 0;
}

// How far P lies from the stretch from A to B.
inline double StretchDistance(const FV3& P, const FV3& A, const FV3& B)
{
    const FV3    D  = B - A;
    const double L2 = RdxSphere::Dot(D, D);
    const double T  = (L2 > 0.0) ? fmax(0.0, fmin(1.0, RdxSphere::Dot(P - A, D) / L2)) : 0.0;
    return RdxSphere::Length(P - (A + D * T));
}

inline long long SideCellOf(double X, double Y, double Z)
{
    return Detail::CellKey((long long)floor(X / SideCell), (long long)floor(Y / SideCell),
                           (long long)floor(Z / SideCell));
}

// Keys sorted, the least first, over Workers threads: pieces sorted each on its own (Detail::SortSides), then merged two
// by two into Room and back - equal keys being the same number, the order one sort gives. Few keys, one thread or no
// room for the copy: one sort.
inline void SortOnThreads(TList<long long>& Keys, int Workers, TList<long long>& Room)
{
    const int N = Keys.Num;
    const int P = (Workers < 1) ? 1 : ((Workers > RdxSphere::WorkersMost) ? RdxSphere::WorkersMost : Workers);
    if (N < 32768 || P < 2 || !Room.Resize(N))
    {
        Detail::SortSides(Keys.Data, N);
        return;
    }
    int B[RdxSphere::WorkersMost + 1];
    for (int i = 0; i <= P; i++)
        B[i] = (int)((long long)N * i / P);
    RdxSphere::OnThreads(P, 1, P, [&](int i, int) { Detail::SortSides(Keys.Data + B[i], B[i + 1] - B[i]); });
    long long* From = Keys.Data;
    long long* To   = Room.Data;
    for (int Width = 1; Width < P; Width *= 2)
    {
        RdxSphere::OnThreads((P + 2 * Width - 1) / (2 * Width), 1, P, [&](int k, int) {
            const int i  = k * 2 * Width;
            const int Lo = B[i], Mid = B[(i + Width < P) ? i + Width : P], Hi = B[(i + 2 * Width < P) ? i + 2 * Width : P];
            int       a = Lo, b = Mid, o = Lo;
            while (a < Mid && b < Hi)
                To[o++] = (From[b] < From[a]) ? From[b++] : From[a++];
            while (a < Mid)
                To[o++] = From[a++];
            while (b < Hi)
                To[o++] = From[b++];
        });
        long long* const Was = From;
        From                 = To;
        To                   = Was;
    }
    if (From != Keys.Data)
        memcpy(Keys.Data, From, sizeof(long long) * (size_t)N);
}

// The cells of the box Lo..Hi, Cell wide, each passed to Visit; false where the box spans more than Most of them, or
// Visit says so.
template <class FVisit>
inline bool EachCell(const FV3& Lo, const FV3& Hi, double Cell, long long Most, FVisit Visit)
{
    const long long X0 = (long long)floor(Lo.X / Cell), X1 = (long long)floor(Hi.X / Cell);
    const long long Y0 = (long long)floor(Lo.Y / Cell), Y1 = (long long)floor(Hi.Y / Cell);
    const long long Z0 = (long long)floor(Lo.Z / Cell), Z1 = (long long)floor(Hi.Z / Cell);
    if ((X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1) > Most)
        return false;
    for (long long cz = Z0; cz <= Z1; cz++)
        for (long long cy = Y0; cy <= Y1; cy++)
            for (long long cx = X0; cx <= X1; cx++)
                if (!Visit(Detail::CellKey(cx, cy, cz)))
                    return false;
    return true;
}

// How many cells of the box Lo..Hi, Cell wide, EachCell would pass on: none where the box spans more than Most.
inline long long CellsOf(const FV3& Lo, const FV3& Hi, double Cell, long long Most)
{
    const long long X0 = (long long)floor(Lo.X / Cell), X1 = (long long)floor(Hi.X / Cell);
    const long long Y0 = (long long)floor(Lo.Y / Cell), Y1 = (long long)floor(Hi.Y / Cell);
    const long long Z0 = (long long)floor(Lo.Z / Cell), Z1 = (long long)floor(Hi.Z / Cell);
    const long long N  = (X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1);
    return (N > Most) ? 0 : N;
}

// The cell four times as wide that cell K (Detail::CellKey) lies in - the cells of SideCell or EdgeCell (16) in those
// of StillCell (64): its numbers, 21 bits each, taken back with their signs and floored by four.
inline long long CellFourWide(long long K)
{
    auto Axis = [](long long V) {
        V &= 0x1FFFFF;
        return ((V & 0x100000) ? V - 0x200000 : V) >> 2;
    };
    return Detail::CellKey(Axis(K >> 42), Axis(K >> 21), Axis(K));
}

// The first entry of sorted Keys whose high bits (above Shift) are at least Top's.
inline int FirstAbove(const TList<long long>& Keys, long long Top, int Shift)
{
    int a = 0, b = Keys.Num;
    while (a < b)
    {
        const int m = (a + b) / 2;
        if ((Keys[m] >> Shift) < Top)
            a = m + 1;
        else
            b = m;
    }
    return a;
}

} // namespace BevelDetail

// What the bevel's two looks at the level share, made once (BevelPrepOf): the faces it looks at, each a flat convex
// piece of a wall where the rounding draws it - first the rings of the walls the rounding left as they were (RingFaces
// of them: face f is the ring of polygon RingPoly[f]), then the triangles of the moved walls, the rounding's own where
// it put them, that lie near enough an outward hard side for a band to reach them (face RingFaces + i is triangle
// TriFace[i] of R's, their polygons' in order). Per face its polygon (Owner), its outward normal, and whether the bevel
// may cut it (Still): a still polygon's ring - no sheet, none left to the engine, a ring that neither touches itself
// nor leaves its plane or turns back, the relief's among them -, or a moved wall's triangle with a plane, no sheet's.
// A ring face's ring (RingPlaces, from RingFirst for RingCount) is its polygon's with every place of a moved wall's
// triangles that lies on one of its sides between its places: the rounding cuts a moved wall's side along a still
// one's at places of its own - T-junctions on a straight side while nothing there moves -, and a corner the bevel moves
// turns both sides alike only where both have them. Every face's sides, from place U to V, one number each, sorted -
// the face across a side is the one with the side from V to U. And per place whether a moved wall's triangle that is
// no face has it (Unseen): a point that moves, put on a side of one, would open a crack there.
struct FBevelPrep
{
    int                  RingFaces = 0;
    TList<int>           RingPoly, TriFace, Owner;
    TList<int>           RingFirst, RingCount, RingPlaces;
    TList<FV3>           Normal;
    TList<unsigned char> Still, Unseen;
    TList<long long>     SideKeys;
};

// A normal of length one in a word, each of its numbers in ten bits, as the card takes it (RenderDevice.cpp's
// PackNormal) - never 0, which only a normal of no length along all three axes the wrong way would pack to.
inline unsigned int PackedNormalOf(const FV3& N)
{
    auto Ten = [](double V) {
        const int Q = (int)floor((V + 1.0) * 511.5 + 0.5);
        return (unsigned int)((Q < 0) ? 0 : (Q > 1023) ? 1023 : Q);
    };
    return Ten(N.X) | (Ten(N.Y) << 10) | (Ten(N.Z) << 20);
}

// Face f's count of corners, and its corner i - a place -, in order round it: a ring face's ring with the T-junctions
// put in, a moved wall's triangle's three.
inline int CornersOf(const FBevelPrep& P, const FResult&, int f) { return (f < P.RingFaces) ? P.RingCount[f] : 3; }
inline int CornerOf(const FBevelPrep& P, const FResult& R, int f, int i)
{
    return (f < P.RingFaces) ? P.RingPlaces[P.RingFirst[f] + i] : R.Tri[P.TriFace[f - P.RingFaces] * 3 + i];
}

// The middle of face f where it is drawn: a point of it off every side, the face being convex.
inline FV3 FaceMiddle(const FBevelPrep& P, const FResult& R, int f)
{
    const int C = CornersOf(P, R, f);
    FV3       M = RdxSphere::V3(0.0, 0.0, 0.0);
    for (int i = 0; i < C; i++)
        M = M + R.Moved[CornerOf(P, R, f, i)] * (1.0 / C);
    return M;
}

// The level's FBevelPrep into P, the rings' shapes over the rounding's threads. Angle the rounding's, Radius the ball's:
// how far round an outward hard side the moved walls' triangles are taken. False where there was no memory for it, or
// the level is too large for the sides' keys.
inline bool BevelPrepOf(const FPolyIn* Polys, int NumPolys, double Angle, double Radius, const FResult& R,
                        FBevelPrep& P)
{
    using namespace BevelDetail;
    if (R.RingFirst.Num != NumPolys || R.RingCount.Num != NumPolys || R.Orig.Num >= (1 << PlaceBits) ||
        NumPolys >= (1 << FaceBits))
        return false;
    const double Pi        = 3.14159265358979323846;
    const double LeastTurn = Pi - Angle * Pi / 180.0, MostTurn = KnifeDegrees * Pi / 180.0;
    // How far a moved wall's surface turns off its polygon's plane at most: the rounding turns it half a soft edge's
    // turn there, and a soft edge turns by less than the least hard turn. A hard side of a moved wall's turns by its
    // polygons' turn and up to that much more or less - one side moved; both, rarely more.
    const double OneSide = 0.5 * LeastTurn;
    // Per polygon: a wall with a ring; one the rounding moved, drawn as its triangles; one left to the engine.
    TList<unsigned char> Wall, Moved, Left;
    if (!Wall.Resize(NumPolys) || !Moved.Resize(NumPolys) || !Left.Resize(NumPolys))
        return false;
    for (int p = 0; p < NumPolys; p++)
    {
        Wall[p]  = ((Polys[p].Use & RdxSphere::Use_Wall) && R.RingCount[p] >= 3) ? 1 : 0;
        Moved[p] = (Wall[p] && p < R.PolyTris.Num && R.PolyTris[p] > 0) ? 1 : 0;
        Left[p]  = 0;
    }
    for (int k = 0; k < R.Left.Num; k++)
        if (R.Left[k] >= 0 && R.Left[k] < NumPolys)
        {
            Left[R.Left[k]]  = 1;
            Moved[R.Left[k]] = 0;
        }

    // The walls' ring sides, place U to V, sorted: the outward hard ones between two walls by the polygons' planes,
    // their turn widened by OneSide, mark round them in cells how far a band could reach - the band's width at that
    // turn and a radius more.
    TList<long long> RingKeys, Room;
    for (int p = 0; p < NumPolys; p++)
    {
        if (!Wall[p])
            continue;
        const int F = R.RingFirst[p], C = R.RingCount[p];
        for (int i = 0; i < C; i++)
        {
            const long long U = R.Ring[F + i], V = R.Ring[F + (i + 1) % C];
            if (U != V && !RingKeys.Add((U << (PlaceBits + FaceBits)) | (V << FaceBits) | p))
                return false;
        }
    }
    SortOnThreads(RingKeys, R.Workers, Room);
    Room.Free();
    const long long FaceMask = (1LL << FaceBits) - 1;
    TList<FV3>      ReachLo, ReachHi;
    Detail::FCells  Reach;
    if (!Reach.Init(RingKeys.Num / 4 + 64))
        return false;
    for (int k = 0; k < RingKeys.Num; k++)
    {
        const long long U = RingKeys[k] >> (PlaceBits + FaceBits), V = (RingKeys[k] >> FaceBits) & ((1LL << PlaceBits) - 1);
        const int       p = (int)(RingKeys[k] & FaceMask);
        if (U >= V)
            continue;
        const long long Back = (V << PlaceBits) | U;
        int             q    = -1;
        for (int a = FirstAbove(RingKeys, Back, FaceBits); a < RingKeys.Num && (RingKeys[a] >> FaceBits) == Back && q < 0;
             a++)
            if ((int)(RingKeys[a] & FaceMask) != p)
                q = (int)(RingKeys[a] & FaceMask);
        if (q < 0)
            continue;
        const FV3    Np = NormalOf(Polys[p]), Nq = NormalOf(Polys[q]);
        const double Turn = acos(fmax(-1.0, fmin(1.0, RdxSphere::Dot(Np, Nq))));
        if (!(Turn + OneSide > LeastTurn))
            continue;
        const FV3 Way  = RdxBevel::Unit(R.Orig[(int)V] - R.Orig[(int)U]);
        FV3       Into = RdxBevel::Unit(RdxSphere::Cross(Np, Way));
        if (RdxSphere::Dot(Into, RingMiddle(R, p) - R.Orig[(int)U]) < 0.0)
            Into = Into * -1.0;
        if (RdxSphere::Dot(Into, Nq) >= 0.0)
            continue;
        const double Far = Radius * tan(0.5 * MinOf(Turn + OneSide, MostTurn)) + Radius;
        const FV3&   A   = R.Moved[(int)U];
        const FV3&   B   = R.Moved[(int)V];
        const int    s   = ReachLo.Num;
        if (!ReachLo.Add(RdxSphere::V3(MinOf(A.X, B.X) - Far, MinOf(A.Y, B.Y) - Far, MinOf(A.Z, B.Z) - Far)) ||
            !ReachHi.Add(RdxSphere::V3(MaxOf(A.X, B.X) + Far, MaxOf(A.Y, B.Y) + Far, MaxOf(A.Z, B.Z) + Far)))
            return false;
        bool bOk = true;
        if (!EachCell(ReachLo[s], ReachHi[s], ReachCell, 1 << 16, [&](long long K) { return bOk = Reach.Add(K, s); }) &&
            !bOk)
            return false;
    }
    RingKeys.Free();
    auto Reached = [&](const FV3& Lo, const FV3& Hi) {
        bool bIn = false;
        EachCell(Lo, Hi, ReachCell, 1 << 16, [&](long long K) {
            for (int g = Reach.Head[Reach.Bucket(K)]; g >= 0 && !bIn; g = Reach.Next[g])
            {
                const int s = Reach.Item[g];
                bIn = Reach.Key[g] == K && Lo.X <= ReachHi[s].X && Hi.X >= ReachLo[s].X && Lo.Y <= ReachHi[s].Y &&
                      Hi.Y >= ReachLo[s].Y && Lo.Z <= ReachHi[s].Z && Hi.Z >= ReachLo[s].Z;
            }
            return !bIn;
        });
        return bIn;
    };

    // The faces: the rings of the walls drawn as they were, then the moved walls' triangles a band can reach.
    P.RingFaces = 0;
    P.RingPoly.Clear();
    P.TriFace.Clear();
    P.Owner.Clear();
    P.Normal.Clear();
    P.Still.Clear();
    for (int p = 0; p < NumPolys; p++)
    {
        if (!Wall[p] || Moved[p])
            continue;
        const unsigned char bCut = (!(Polys[p].Use & RdxSphere::Use_Sheet) && !Left[p]) ? 1 : 0;
        if (!P.RingPoly.Add(p) || !P.Owner.Add(p) || !P.Normal.Add(NormalOf(Polys[p])) || !P.Still.Add(bCut))
            return false;
    }
    P.RingFaces = P.Owner.Num;
    RdxSphere::OnThreads(P.RingFaces, 256, R.Workers, [&](int f, int) {
        const int p = P.RingPoly[f];
        if (P.Still[f] && (RingTouchesItself(R, p) || !RingFlatAndConvex(R, Polys[p], p)))
            P.Still[f] = 0;
    });

    // The moved walls' triangles a band can reach, and with them every one that has a place of theirs: a band's point
    // goes onto a side they share, and a place that moves turns both - neither may be one no face is.
    const int            NumTris = R.Tri.Num / 3;
    TList<unsigned char> TriIn, NearPlace;
    if (!TriIn.Resize(NumTris) || !NearPlace.Resize(R.Orig.Num))
        return false;
    for (int t = 0; t < NumTris; t++)
        TriIn[t] = 0;
    for (int w = 0; w < R.Orig.Num; w++)
        NearPlace[w] = 0;
    // The moved walls' triangles a band can reach, over the threads - each triangle its own polygon's, written by one
    // thread alone -, then their places, in one go after.
    RdxSphere::OnThreads(NumPolys, 64, R.Workers, [&](int p, int) {
        if (!Moved[p])
            return;
        for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
        {
            const FV3& A  = R.Moved[R.Tri[t * 3]];
            const FV3& B  = R.Moved[R.Tri[t * 3 + 1]];
            const FV3& Cc = R.Moved[R.Tri[t * 3 + 2]];
            const FV3  Lo = RdxSphere::V3(MinOf(A.X, MinOf(B.X, Cc.X)), MinOf(A.Y, MinOf(B.Y, Cc.Y)),
                                          MinOf(A.Z, MinOf(B.Z, Cc.Z)));
            const FV3  Hi = RdxSphere::V3(MaxOf(A.X, MaxOf(B.X, Cc.X)), MaxOf(A.Y, MaxOf(B.Y, Cc.Y)),
                                          MaxOf(A.Z, MaxOf(B.Z, Cc.Z)));
            if (Reached(Lo, Hi))
                TriIn[t] = 1;
        }
    });
    for (int p = 0; p < NumPolys; p++)
        if (Moved[p])
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                if (TriIn[t] == 1)
                    for (int k = 0; k < 3; k++)
                        NearPlace[R.Tri[t * 3 + k]] = 1;
    for (int p = 0; p < NumPolys; p++)
        if (Moved[p])
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                if (!TriIn[t] && (NearPlace[R.Tri[t * 3]] || NearPlace[R.Tri[t * 3 + 1]] || NearPlace[R.Tri[t * 3 + 2]]))
                    TriIn[t] = 2;
    NearPlace.Free();

    if (!P.Unseen.Resize(R.Orig.Num))
        return false;
    for (int w = 0; w < R.Orig.Num; w++)
        P.Unseen[w] = 0;
    for (int p = 0; p < NumPolys; p++)
    {
        if (!Moved[p])
            continue;
        const FV3    Np = NormalOf(Polys[p]);
        const double D  = RdxSphere::Dot(Np, R.Orig[R.Ring[R.RingFirst[p]]]);
        // The way the polygon's ring goes round its normal: its triangles go the same way.
        FV3       Sum = RdxSphere::V3(0.0, 0.0, 0.0);
        const int F = R.RingFirst[p], C = R.RingCount[p];
        for (int i = 1; i + 1 < C; i++)
        {
            const FV3& O = R.Orig[R.Ring[F]];
            Sum          = Sum + RdxSphere::Cross(R.Orig[R.Ring[F + i]] - O, R.Orig[R.Ring[F + i + 1]] - O);
        }
        const double Way = (RdxSphere::Dot(Sum, Np) >= 0.0) ? 1.0 : -1.0;
        for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
        {
            const int  a = R.Tri[t * 3], b = R.Tri[t * 3 + 1], c = R.Tri[t * 3 + 2];
            const FV3& A = R.Moved[a];
            const FV3& B = R.Moved[b];
            const FV3& Cc = R.Moved[c];
            if (!TriIn[t])
            {
                P.Unseen[a] = P.Unseen[b] = P.Unseen[c] = 1;
                continue;
            }
            // On the polygon's plane: its normal. Off it, the triangle's own - where it has one: a sliver's corners
            // all but on one line make none, and no band cuts it (Still 2), its sides found by the polygon's - but it
            // takes in the points the faces beside it put on its sides, for the level to stay closed there.
            FV3          N     = Np;
            int          Cut   = (Polys[p].Use & RdxSphere::Use_Sheet) ? 0 : 1;
            const FV3    X     = RdxSphere::Cross(B - A, Cc - A) * Way;
            const double Area2 = RdxSphere::Length(X);
            const double Long  = MaxOf(RdxSphere::Length(B - A), MaxOf(RdxSphere::Length(Cc - B), RdxSphere::Length(A - Cc)));
            if (!(Area2 > SliverShare * Long * Long))
                Cut = Cut ? 2 : 0;
            else if (fabs(RdxSphere::Dot(Np, A) - D) > OnPlane || fabs(RdxSphere::Dot(Np, B) - D) > OnPlane ||
                     fabs(RdxSphere::Dot(Np, Cc) - D) > OnPlane)
                N = X * (1.0 / Area2);
            if (P.Owner.Num >= (1 << FaceBits) - 1 || !P.TriFace.Add(t) || !P.Owner.Add(p) || !P.Normal.Add(N) ||
                !P.Still.Add((unsigned char)Cut))
                return false;
        }
    }

    // Every ring face's ring: its polygon's, with every place of the walls' that lies on one of its sides between its
    // corners (within OnEdge, as the rounding's T-junctions do) put in, in order along it. The rounding cuts a moved
    // wall's side along a still one's at places of its own - T-junctions on a straight side while nothing there moves -,
    // and a corner the bevel moves turns both sides alike only where both have them. A moved wall's triangle keeps its
    // three corners: its wall's triangles close it as the rounding drew them, and a place put in on a side of one would
    // bend it off the triangles beside it.
    {
        const int NumFaces = P.Owner.Num;
        auto      BaseCount  = [&](int f) { return (f < P.RingFaces) ? R.RingCount[P.RingPoly[f]] : 3; };
        auto      BaseCorner = [&](int f, int i) {
            return (f < P.RingFaces) ? R.Ring[R.RingFirst[P.RingPoly[f]] + i] : R.Tri[P.TriFace[f - P.RingFaces] * 3 + i];
        };
        auto CellOf = [](const FV3& Q) {
            return Detail::CellKey((long long)floor(Q.X / TCell), (long long)floor(Q.Y / TCell),
                                   (long long)floor(Q.Z / TCell));
        };
        // The places by cells: every corner of a face and of a moved wall's triangle, once.
        TList<unsigned char> Looked;
        Detail::FCells       PlaceGrid;
        if (!Looked.Resize(R.Orig.Num) || !PlaceGrid.Init(R.Orig.Num / 2 + 64))
            return false;
        for (int w = 0; w < R.Orig.Num; w++)
            Looked[w] = 0;
        auto Put = [&](int w) {
            if (Looked[w])
                return true;
            Looked[w] = 1;
            return PlaceGrid.Add(CellOf(R.Moved[w]), w);
        };
        for (int f = 0; f < NumFaces; f++)
            for (int i = 0; i < BaseCount(f); i++)
                if (!Put(BaseCorner(f, i)))
                    return false;
        for (int p = 0; p < NumPolys; p++)
            if (Moved[p])
                for (int t = R.PolyFirst[p] * 3; t < (R.PolyFirst[p] + R.PolyTris[p]) * 3; t++)
                    if (!Put(R.Tri[t]))
                        return false;
        // Along each face's side, the places in the cells it goes through - a point of it every half a cell, the
        // cells round it within half a step and OnEdge -, those lying on it put in its ring, in order along it.
        struct FOn
        {
            double T;
            int    Place;
        };
        TList<FOn> Ons;
        // The ring's own corners: none is put in on a side of it, though one lie on it.
        TList<unsigned char> Own;
        if (!P.RingFirst.Resize(P.RingFaces) || !P.RingCount.Resize(P.RingFaces) || !Own.Resize(R.Orig.Num))
            return false;
        for (int w = 0; w < R.Orig.Num; w++)
            Own[w] = 0;
        // Every face's sides as they come (Whole): a side another face has the other way round, place for place, is
        // where the two meet whole - a place of a third polygon on it only touches it there, and the side takes none in.
        TList<long long> Whole;
        for (int f = 0; f < NumFaces; f++)
            for (int i = 0; i < BaseCount(f); i++)
            {
                const long long U = BaseCorner(f, i), V = BaseCorner(f, (i + 1) % BaseCount(f));
                if (U != V && !Whole.Add((U << PlaceBits) | V))
                    return false;
            }
        SortOnThreads(Whole, R.Workers, Room);
        auto MetWhole = [&](long long U, long long V) {
            const long long K = (U << PlaceBits) | V;
            const int       a = FirstAbove(Whole, K, 0);
            return a < Whole.Num && Whole[a] == K;
        };
        P.RingPlaces.Clear();
        for (int f = 0; f < P.RingFaces; f++)
        {
            const int C    = BaseCount(f);
            P.RingFirst[f] = P.RingPlaces.Num;
            for (int i = 0; i < C; i++)
                Own[BaseCorner(f, i)] = 1;
            for (int i = 0; i < C; i++)
            {
                const int U = BaseCorner(f, i), V = BaseCorner(f, (i + 1) % C);
                if (!P.RingPlaces.Add(U))
                    return false;
                const FV3    A = R.Moved[U], D = R.Moved[V] - A;
                const double L2 = RdxSphere::Dot(D, D);
                if (!(L2 > 0.0) || MetWhole(V, U))
                    continue;
                Ons.Clear();
                const int Steps = (int)(sqrt(L2) / (0.5 * TCell)) + 1;
                long long Last[8];
                int       LastNum = 0;
                for (int k = 0; k <= Steps; k++)
                {
                    // The cells within half a step and OnEdge of the point: a place on the side lies that near one of
                    // them - the side may go just through a cell's corner, between two points' own cells.
                    const FV3    Q   = A + D * ((double)k / Steps);
                    const double Box = 0.5 * sqrt(L2) / Steps + OnEdge;
                    const FV3    Lo  = RdxSphere::V3(Q.X - Box, Q.Y - Box, Q.Z - Box);
                    const FV3    Hi  = RdxSphere::V3(Q.X + Box, Q.Y + Box, Q.Z + Box);
                    long long Now[8];
                    int       NowNum = 0;
                    bool      bOk    = true;
                    EachCell(Lo, Hi, TCell, 8, [&](long long K) {
                        Now[NowNum++] = K;
                        // A cell the point before had already: looked in then.
                        for (int j = 0; j < LastNum; j++)
                            if (Last[j] == K)
                                return true;
                        for (int g = PlaceGrid.Head[PlaceGrid.Bucket(K)]; g >= 0; g = PlaceGrid.Next[g])
                        {
                            const int w = PlaceGrid.Item[g];
                            if (PlaceGrid.Key[g] != K || Own[w])
                                continue;
                            const FV3&   X = R.Moved[w];
                            const double T = RdxSphere::Dot(X - A, D) / L2;
                            if (!(T > 0.0 && T < 1.0) || RdxSphere::Length(X - (A + D * T)) > OnEdge)
                                continue;
                            const FOn On = { T, w };
                            if (!Ons.Add(On))
                                return bOk = false;
                        }
                        return true;
                    });
                    if (!bOk)
                        return false;
                    for (int j = 0; j < NowNum; j++)
                        Last[j] = Now[j];
                    LastNum = NowNum;
                }
                // Along it in order, each place once.
                for (int a = 1; a < Ons.Num; a++)
                    for (int b = a; b > 0 && Ons[b].T < Ons[b - 1].T; b--)
                    {
                        const FOn Tmp = Ons[b];
                        Ons[b]        = Ons[b - 1];
                        Ons[b - 1]    = Tmp;
                    }
                int Kept = 0;
                for (int a = 0; a < Ons.Num; a++)
                {
                    bool bHad = false;
                    for (int b = P.RingPlaces.Num - 1; b >= P.RingFirst[f] && !bHad; b--)
                        bHad = P.RingPlaces[b] == Ons[a].Place;
                    for (int b = 0; b < Kept && !bHad; b++)
                        bHad = Ons[b].Place == Ons[a].Place;
                    if (!bHad)
                        Ons[Kept++] = Ons[a];
                }
                Ons.Num = Kept;
                // Where a face across has a stretch of the side whole, from one of its places to a later one, the
                // places between stay out: they only touch the side there - a corner of a wall that leaves it -, and
                // put in they would part the ring's side from that face's, the points a band puts on the one never
                // reaching the other.
                auto Along = [&](int s) { return (s == 0) ? U : ((s <= Ons.Num) ? Ons[s - 1].Place : V); };
                for (int s = 0; s <= Ons.Num;)
                {
                    int e = Ons.Num + 1;
                    while (e > s + 1 && !MetWhole(Along(e), Along(s)))
                        e--;
                    for (int k = s + 1; k < e; k++)
                        Ons[k - 1].Place = -1;
                    s = (e > s + 1) ? e : s + 1;
                }
                for (int a = 0; a < Ons.Num; a++)
                    if (Ons[a].Place >= 0 && !P.RingPlaces.Add(Ons[a].Place))
                        return false;
            }
            P.RingCount[f] = P.RingPlaces.Num - P.RingFirst[f];
            for (int i = 0; i < C; i++)
                Own[BaseCorner(f, i)] = 0;
        }
    }

    // Every face's sides.
    P.SideKeys.Clear();
    for (int f = 0; f < P.Owner.Num; f++)
    {
        const int C = CornersOf(P, R, f);
        for (int i = 0; i < C; i++)
        {
            const long long U = CornerOf(P, R, f, i), V = CornerOf(P, R, f, (i + 1) % C);
            if (U != V && !P.SideKeys.Add((U << (PlaceBits + FaceBits)) | (V << FaceBits) | f))
                return false;
        }
    }
    SortOnThreads(P.SideKeys, R.Workers, Room);
    return true;
}

// The edges to bevel (above) with Radius into Edges, and what was found into S, from the level's Prep (BevelPrepOf).
// Angle the rounding's (SpherifierAngle): two faces meeting at it or wider are one surface, the rounding's to round.
// False where there was no memory for it.
inline bool BevelEdgesOf(const FPolyIn* Polys, double Angle, double Radius, const FResult& R, const FBevelPrep& Prep,
                         TList<RdxBevel::FEdge>& Edges, FBevelStats& S)
{
    using namespace BevelDetail;
    S = FBevelStats();
    Edges.Clear();
    const int                   NumFaces = Prep.Owner.Num;
    const TList<unsigned char>& Still    = Prep.Still;
    const TList<long long>&     Keys     = Prep.SideKeys;
    auto Across = [&](long long U, long long V, int f) {
        const long long Lo = (V << (PlaceBits + FaceBits)) | (U << FaceBits);
        int             a = 0, b = Keys.Num;
        while (a < b)
        {
            const int m = (a + b) / 2;
            if (Keys[m] < Lo)
                a = m + 1;
            else
                b = m;
        }
        for (; a < Keys.Num && (Keys[a] >> FaceBits) == (Lo >> FaceBits); a++)
        {
            const int g = (int)(Keys[a] & ((1LL << FaceBits) - 1));
            if (g != f)
                return g;
        }
        return -1;
    };

    // The hard sides, each once - from the face that goes along it from its lower place - and of them the ones to
    // bevel.
    const double Pi        = 3.14159265358979323846;
    const double LeastTurn = Pi - Angle * Pi / 180.0, MostTurn = KnifeDegrees * Pi / 180.0;
    // A piece of the faces at a time, in order, over the threads, each piece's sides into its own list; the pieces'
    // lists then one after another - the faces' order.
    static const int  Pieces = 256;
    TList<FSide>      PieceSides[Pieces];
    const int         PerPiece = (NumFaces + Pieces - 1) / Pieces;
    std::atomic<bool> NoRoom(false);
    RdxSphere::OnThreads(Pieces, 1, R.Workers, [&](int k, int) {
        TList<FSide>& Out  = PieceSides[k];
        const int     From = k * PerPiece, To = (From + PerPiece < NumFaces) ? From + PerPiece : NumFaces;
        for (int f = From; f < To; f++)
        {
            const int C   = CornersOf(Prep, R, f);
            const FV3 Mid = FaceMiddle(Prep, R, f);
            const FV3 Nf  = Prep.Normal[f];
            for (int i = 0; i < C; i++)
            {
                const int U = CornerOf(Prep, R, f, i), V = CornerOf(Prep, R, f, (i + 1) % C);
                if (U >= V)
                    continue;
                const int g = Across(U, V, f);
                if (g < 0)
                    continue;
                const FV3    Ng   = Prep.Normal[g];
                const double Turn = acos(fmax(-1.0, fmin(1.0, RdxSphere::Dot(Nf, Ng))));
                if (!(Turn > LeastTurn))
                    continue;
                // The rounding makes no hard side and no knife's: its faces' sides are hard only where their polygons
                // met at one, its own turn off the plane aside (BevelPrepOf's OneSide) - a fold in its triangles is no
                // edge, and a thin wall's two sides back to back stay a knife's however the rounding turns one.
                const int    Pf = Prep.Owner[f], Pg = Prep.Owner[g];
                const double PolyTurn =
                    acos(fmax(-1.0, fmin(1.0, RdxSphere::Dot(NormalOf(Polys[Pf]), NormalOf(Polys[Pg])))));
                if (Pf == Pg || !(PolyTurn + 0.5 * LeastTurn > LeastTurn) || !(PolyTurn < MostTurn))
                    continue;
                // Outward where the other face turns away from this one's inside, as a box's do.
                const FV3 Way  = RdxBevel::Unit(R.Moved[V] - R.Moved[U]);
                FV3       Into = RdxBevel::Unit(RdxSphere::Cross(Nf, Way));
                if (RdxSphere::Dot(Into, Mid - R.Moved[U]) < 0.0)
                    Into = Into * -1.0;
                FSide Side;
                Side.U    = U;
                Side.V    = V;
                Side.A    = f;
                Side.B    = g;
                Side.bOut = RdxSphere::Dot(Into, Ng) < 0.0;
                RdxBevel::FEdge E;
                Side.bBevel = Side.bOut && Still[f] == 1 && Still[g] == 1 &&
                              RdxBevel::MakeEdge(R.Moved[U], R.Moved[V], Nf, Ng, Mid, LeastTurn, MostTurn, E);
                if (!Out.Add(Side))
                    NoRoom = true;
            }
        }
    });
    if (NoRoom)
        return false;
    TList<FSide> Sides;
    for (int k = 0; k < Pieces; k++)
        for (int i = 0; i < PieceSides[k].Num; i++)
        {
            if (!Sides.Add(PieceSides[k][i]))
                return false;
            S.Sides += PieceSides[k][i].bBevel ? 1 : 0;
        }

    // The hard sides by the cells they reach into, as far round as an end looks (Near) and a thin edge's neighbour
    // lies (twice the radius).
    const double Pad = MaxOf(Near, 2.0 * Radius);
    Detail::FCells Grid;
    if (!Grid.Init(Sides.Num * 4))
        return false;
    for (int h = 0; h < Sides.Num; h++)
    {
        const FV3& A = R.Moved[Sides[h].U];
        const FV3& B = R.Moved[Sides[h].V];
        const long long X0 = (long long)floor((MinOf(A.X, B.X) - Pad) / SideCell),
                        X1 = (long long)floor((MaxOf(A.X, B.X) + Pad) / SideCell);
        const long long Y0 = (long long)floor((MinOf(A.Y, B.Y) - Pad) / SideCell),
                        Y1 = (long long)floor((MaxOf(A.Y, B.Y) + Pad) / SideCell);
        const long long Z0 = (long long)floor((MinOf(A.Z, B.Z) - Pad) / SideCell),
                        Z1 = (long long)floor((MaxOf(A.Z, B.Z) + Pad) / SideCell);
        if ((X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1) > 4096)
            continue;   // a side across the whole level: too long to be an end's or a thin face's neighbour here
        for (long long cz = Z0; cz <= Z1; cz++)
            for (long long cy = Y0; cy <= Y1; cy++)
                for (long long cx = X0; cx <= X1; cx++)
                    if (!Grid.Add(Detail::CellKey(cx, cy, cz), h))
                        return false;
    }

    // The runs of sides to bevel: sides that share a place, lie along one line and between the same two planes.
    TList<int>       Up;
    TList<long long> Ends;
    if (!Up.Resize(Sides.Num))
        return false;
    for (int s = 0; s < Sides.Num; s++)
    {
        Up[s] = s;
        if (Sides[s].bBevel &&
            (!Ends.Add(((long long)Sides[s].U << 32) | s) || !Ends.Add(((long long)Sides[s].V << 32) | s)))
            return false;
    }
    TList<long long> Room;
    SortOnThreads(Ends, R.Workers, Room);
    auto WayOf   = [&](int s) { return RdxBevel::Unit(R.Moved[Sides[s].V] - R.Moved[Sides[s].U]); };
    auto SameRun = [&](int s, int t) {
        if (fabs(RdxSphere::Dot(WayOf(s), WayOf(t))) < SameWay)
            return false;
        const FV3 As = Prep.Normal[Sides[s].A], Bs = Prep.Normal[Sides[s].B];
        const FV3 At = Prep.Normal[Sides[t].A], Bt = Prep.Normal[Sides[t].B];
        return (RdxSphere::Dot(As, At) >= SameWay && RdxSphere::Dot(Bs, Bt) >= SameWay) ||
               (RdxSphere::Dot(As, Bt) >= SameWay && RdxSphere::Dot(Bs, At) >= SameWay);
    };
    for (int a = 0; a < Ends.Num;)
    {
        int b = a + 1;
        while (b < Ends.Num && (Ends[b] >> 32) == (Ends[a] >> 32))
            b++;
        for (int i = a; i < b; i++)
            for (int j = i + 1; j < b; j++)
            {
                const int s = (int)(Ends[i] & 0xFFFFFFFF), t = (int)(Ends[j] & 0xFFFFFFFF);
                if (SameRun(s, t))
                {
                    const int Rs = RunOf(Up, s), Rt = RunOf(Up, t);
                    if (Rs != Rt)
                        Up[(Rs < Rt) ? Rt : Rs] = (Rs < Rt) ? Rs : Rt;
                }
            }
        a = b;
    }

    // Each run's sides together (Order, from RunStart[s] for the run whose first side is s).
    TList<int> RunStart, Order, Cursor;
    if (!RunStart.Resize(Sides.Num + 1) || !Order.Resize(Sides.Num) || !Cursor.Resize(Sides.Num))
        return false;
    for (int s = 0; s <= Sides.Num; s++)
        RunStart[s] = 0;
    for (int s = 0; s < Sides.Num; s++)
        if (Sides[s].bBevel)
            RunStart[RunOf(Up, s) + 1]++;
    for (int s = 0; s < Sides.Num; s++)
        RunStart[s + 1] += RunStart[s];
    for (int s = 0; s < Sides.Num; s++)
        Cursor[s] = RunStart[s];
    for (int s = 0; s < Sides.Num; s++)
        if (Sides[s].bBevel)
            Order[Cursor[RunOf(Up, s)]++] = s;

    // Each run an edge: its line from end to end along the first side's way, its faces those of its first side. The
    // runs looked at over the threads, each by its first side (Kinds: 0 an edge, Made; 2 a free end; 3 thin; 4 none to
    // be made) - each side's run known first (Root) -, and taken in that order.
    TList<int> Root, Firsts;
    if (!Root.Resize(Sides.Num))
        return false;
    for (int s = 0; s < Sides.Num; s++)
    {
        Root[s] = RunOf(Up, s);
        if (RunStart[s + 1] > RunStart[s] && !Firsts.Add(s))
            return false;
    }
    TList<unsigned char>   Kinds;
    TList<RdxBevel::FEdge> Made;
    if (!Kinds.Resize(Firsts.Num) || !Made.Resize(Firsts.Num))
        return false;
    RdxSphere::OnThreads(Firsts.Num, 16, R.Workers, [&](int r, int) {
        const int s = Firsts[r];
        Kinds[r]    = 4;
        const FV3 Way  = WayOf(s);
        const FV3 Base = R.Moved[Sides[s].U];
        double    Lo = 1e300, Hi = -1e300;
        for (int m = RunStart[s]; m < RunStart[s + 1]; m++)
        {
            const FSide& M  = Sides[Order[m]];
            const double Tu = RdxSphere::Dot(R.Moved[M.U] - Base, Way), Tv = RdxSphere::Dot(R.Moved[M.V] - Base, Way);
            Lo = MinOf(Lo, MinOf(Tu, Tv));
            Hi = MaxOf(Hi, MaxOf(Tu, Tv));
        }
        const FSide& First = Sides[s];
        const FV3    NA = Prep.Normal[First.A], NB = Prep.Normal[First.B];
        auto         OwnRun = [&](int h) { return Sides[h].bBevel && Root[h] == s; };

        // A free end: no hard side but the run's own ends or passes within Near of it, or one goes on along the line
        // past it.
        bool bFree = false;
        for (int e = 0; e < 2 && !bFree; e++)
        {
            const FV3       P     = Base + Way * ((e == 0) ? Lo : Hi);
            const long long K     = SideCellOf(P.X, P.Y, P.Z);
            bool            bMeet = false, bOn = false;
            for (int g = Grid.Head[Grid.Bucket(K)]; g >= 0; g = Grid.Next[g])
            {
                if (Grid.Key[g] != K)
                    continue;
                const int h = Grid.Item[g];
                if (OwnRun(h))
                    continue;
                const FV3& A = R.Moved[Sides[h].U];
                const FV3& B = R.Moved[Sides[h].V];
                if (StretchDistance(P, A, B) > Near)
                    continue;
                // Along the line and past the end: the edge goes on, unbevelled.
                const double Out = (e == 0) ? -1.0 : 1.0;
                if (fabs(RdxSphere::Dot(WayOf(h), Way)) >= SameWay &&
                    MaxOf(RdxSphere::Dot(A - P, Way) * Out, RdxSphere::Dot(B - P, Way) * Out) > Near)
                    bOn = true;
                else
                    bMeet = true;
            }
            bFree = bOn || !bMeet;
        }
        if (bFree)
        {
            Kinds[r] = 2;
            return;
        }

        // Short: a run shorter than twice the radius - a thin slab's end, between its two long edges left sharp as thin
        // - has no room for the ball along it: rounded, its corners would stand off the sharp edges they meet. Left sharp,
        // as thin.
        if (Hi - Lo < 2.0 * Radius)
        {
            Kinds[r] = 3;
            return;
        }

        // Thin: an outward hard side on one of the run's planes, along it, beside it over Near or more of its length,
        // nearer than twice the radius.
        bool bThin = false;
        const int Steps = ((int)((Hi - Lo) / (SideCell * 0.5)) + 1 > 1) ? (int)((Hi - Lo) / (SideCell * 0.5)) + 1 : 1;
        for (int k = 0; k <= Steps && !bThin; k++)
        {
            const FV3       P = Base + Way * (Lo + (Hi - Lo) * k / Steps);
            const long long K = SideCellOf(P.X, P.Y, P.Z);
            for (int g = Grid.Head[Grid.Bucket(K)]; g >= 0 && !bThin; g = Grid.Next[g])
            {
                if (Grid.Key[g] != K)
                    continue;
                const int h = Grid.Item[g];
                if (OwnRun(h) || !Sides[h].bOut || fabs(RdxSphere::Dot(WayOf(h), Way)) < Parallel)
                    continue;
                const FV3& A = R.Moved[Sides[h].U];
                const FV3& B = R.Moved[Sides[h].V];
                const FV3  Na = Prep.Normal[Sides[h].A], Nb = Prep.Normal[Sides[h].B];
                auto OnPlaneOf = [&](const FV3& N) {
                    return (RdxSphere::Dot(Na, N) >= SameWay || RdxSphere::Dot(Nb, N) >= SameWay) &&
                           fabs(RdxSphere::Dot(A - Base, N)) <= OnPlane;
                };
                if (!OnPlaneOf(NA) && !OnPlaneOf(NB))
                    continue;
                const double Ta = RdxSphere::Dot(A - Base, Way), Tb = RdxSphere::Dot(B - Base, Way);
                if (MinOf(Hi, MaxOf(Ta, Tb)) - MaxOf(Lo, MinOf(Ta, Tb)) < Near)
                    continue;
                if (RdxSphere::Length((A - Base) - Way * Ta) < 2.0 * Radius)
                    bThin = true;
            }
        }
        if (bThin)
        {
            Kinds[r] = 3;
            return;
        }
        if (RdxBevel::MakeEdge(Base + Way * Lo, Base + Way * Hi, NA, NB, FaceMiddle(Prep, R, First.A), LeastTurn,
                               MostTurn, Made[r]))
            Kinds[r] = 0;
    });
    for (int r = 0; r < Firsts.Num; r++)
    {
        S.LeftFree += (Kinds[r] == 2) ? 1 : 0;
        S.LeftThin += (Kinds[r] == 3) ? 1 : 0;
        if (Kinds[r] == 0 && !Edges.Add(Made[r]))
            return false;
    }
    S.Edges = Edges.Num;
    return true;
}

// What the bevel made of the edges: the polygons cut, the places it put in, the triangles of those polygons before and
// after, and the edges left sharp at the last - one of whose points would have moved a polygon the bevel may not
// touch (moved by the rounding, a sheet, one the relief may be drawn on), or would have had no worn solid under it -
// and how many times it was made, each time without the edges the one before left sharp; and the ring sides a band's
// line went across that it had not been put on first (none should be).
struct FBevelMade
{
    int Polygons = 0, Points = 0, TrisBefore = 0, TrisAfter = 0;
    int LeftTouching = 0, LeftWorn = 0, Rounds = 0, SidesMissed = 0;
    int LeftMovers = 0;   // of LeftTouching, those whose point would have moved a mover's polygon (MorePolys)
    int PiecesFlat = 0;   // pieces whose corners were left on one line: none should be
    // Pieces a triangle of which the moves turned over, and of them those cut again where their places moved.
    int PiecesTurned = 0, PiecesCutMoved = 0;
    // How long its parts took, milliseconds, by the rounding's clock (FResult::Seconds; none: not measured): what its
    // two looks share (0, BevelPrepOf), the edges (1), the polygons' lists (2), and over all its makings the lines and
    // the points they put on the rings (3), the places' moves and the edges left sharp by them (4), the cuts into
    // triangles (5), the cuts' own points' moves and the triangles handed over (6); and how many makings came as far as
    // the cuts.
    static const int Parts = 7;
    double PartMs[Parts] = {};
    int    CutRounds     = 0;
    int    CutAgain      = 0;   // faces cut again in their turn: one of their run before them made a place they would take
};

namespace BevelDetail
{

static const double OnLine      = WeldDist;   // units: on a band's line this near it; two places this near are one
static const double StillCell   = 64.0;   // units: the cells the still polygons are found by, four EdgeCells wide
static const double EdgeCell    = 16.0;   // units: the cells the edges' bands are found by
static const int    MostAtPoint = 16;     // edges whose bands one place is looked for in - more than its planes can be
static const int    MostRounds  = 12;     // makings, each one without the edges the one before left sharp
// The lines a band has on each of its two faces, at its width and at each step of it nearer the edge: a right angle's
// round in four steps.
static const int    BandSlices  = 2;
static const int    LineBits    = 22;     // a band's line in a key: 2 * BandSlices an edge
static const int    OnSideBits  = 21;     // a point on a side in a sorting key: its number, after a side's 42 bits
static const double NewCellSize = 0.01;   // units: the small cells the new places are found by, near one another
static const int    NewSeenBits = 23;     // the small cells' bits (NewSeen): 8 M of them, a megabyte
static const int    DroppedNearBits = 20; // the edges' cells' bits (DroppedNear): 1 M of them, 128 KB
// units: two edges' lines this near each other all over a polygon are cut along once there. The pieces of one straight
// edge the level's polygons split it into lie a few thousandths apart, their lines crossing at the flattest angles, and
// the strips between them narrower than OnLine where a third line cuts them; the places move by every band all the
// same.
static const double LinesOne = 0.05;

// Convex piece Ids (places, in the order of its polygon's ring) into triangles of Tri, three places each, none of no
// area, every corner of the piece a corner of one - a corner on a side's line, a neighbour's point, too, so the side
// stays whole. A piece of up to MostShaped corners is cut the way, of all the ways, whose worst triangle is the best
// shaped - its least height over its longest side the largest, no height within OnLine (the classic triangulation of a
// convex polygon by its sub-polygons: the chord from corner i to corner j cut by the best corner k between) -: a sliver
// from a point a band put on a side to a corner far along it turns over when the band moves its points a little, where
// a triangle to a corner across stands it. Else, or where every way has such a height, a corner is cut off where its
// two neighbours make a triangle of area with it: first one after which a run of corners on one line starts - cut, the
// run's first corner turns into a corner the next cut takes, so the run is fanned from off its line -, then any.
// At(place) where a place is; Turn the ring's way round its normal N (+1, -1). False where only corners on one line are
// left: a piece of no area.
template <class FAt>
inline bool CutIntoTriangles(FAt At, TList<int>& Ids, const FV3& N, double Turn, TList<int>& Tri)
{
    // A corner within OnLine of its neighbours' line lies on it: a band's point on a cut's line the level's numbers put
    // a hair off it, whose sliver would open when the three places move apart.
    auto Corner = [&](int a, int b, int c) {
        const FV3    A = At(a), B = At(b), C = At(c);
        const double Area = RdxSphere::Dot(RdxSphere::Cross(B - A, C - B), N) * Turn;
        const double L    = RdxSphere::Length(C - A);
        if (L > 0.0 && RdxSphere::Length(RdxSphere::Cross(B - A, C - A)) <= OnLine * L)
            return false;
        return Area > 1e-9 * RdxSphere::Length(B - A) * RdxSphere::Length(C - B);
    };
    // Its tables live on the thread's stack and its time grows with the corners' cube; a piece is a few corners, and one
    // with more is cut the other way rather than not at all.
    static const int MostShaped = 40;
    const int        Num        = Ids.Num;
    if (Num > 3 && Num <= MostShaped)
    {
        FV3 P[MostShaped];
        for (int c = 0; c < Num; c++)
            P[c] = At(Ids[c]);
        // Triangle i, k, j's shape, squared: (twice its area over its longest side's square) squared; -1 where it is
        // none - turned the ring's other way, or a height within OnLine.
        auto Shape = [&](int i, int k, int j) {
            const FV3 X = RdxSphere::Cross(P[k] - P[i], P[j] - P[i]);
            if (RdxSphere::Dot(X, N) * Turn <= 0.0)
                return -1.0;
            const FV3    Ik = P[k] - P[i], Kj = P[j] - P[k], Ji = P[i] - P[j];
            const double Long = MaxOf(RdxSphere::Dot(Ik, Ik), MaxOf(RdxSphere::Dot(Kj, Kj), RdxSphere::Dot(Ji, Ji)));
            const double A2   = RdxSphere::Dot(X, X);
            if (A2 <= OnLine * OnLine * Long)
                return -1.0;
            return A2 / (Long * Long);
        };
        // Best[i][j]: the worst shape of the best way to cut corners i to j (a side: none, the largest number); -1 none.
        double        Best[MostShaped][MostShaped];
        unsigned char Pick[MostShaped][MostShaped];
        for (int i = 0; i + 1 < Num; i++)
            Best[i][i + 1] = 1e300;
        for (int Len = 2; Len < Num; Len++)
            for (int i = 0; i + Len < Num; i++)
            {
                const int j = i + Len;
                double    B = -1.0;
                int       K = i + 1;
                for (int k = i + 1; k < j; k++)
                {
                    if (Best[i][k] < 0.0 || Best[k][j] < 0.0)
                        continue;
                    const double S = Shape(i, k, j);
                    if (S < 0.0)
                        continue;
                    const double W = MinOf(S, MinOf(Best[i][k], Best[k][j]));
                    if (W > B)
                    {
                        B = W;
                        K = k;
                    }
                }
                Best[i][j] = B;
                Pick[i][j] = (unsigned char)K;
            }
        if (Best[0][Num - 1] >= 0.0)
        {
            const int Tri0 = Tri.Num;
            int       Stack[4 * MostShaped], Top = 0;
            Stack[Top++] = 0;
            Stack[Top++] = Num - 1;
            while (Top > 0)
            {
                const int j = Stack[--Top], i = Stack[--Top];
                if (j - i < 2)
                    continue;
                const int k = Pick[i][j];
                if (!Tri.Add(Ids[i]) || !Tri.Add(Ids[k]) || !Tri.Add(Ids[j]))
                {
                    Tri.Num = Tri0;
                    return false;
                }
                Stack[Top++] = i;
                Stack[Top++] = k;
                Stack[Top++] = k;
                Stack[Top++] = j;
            }
            return true;
        }
    }
    while (Ids.Num > 3)
    {
        const int n   = Ids.Num;
        int       Ear = -1;
        for (int k = 0; k < n && Ear < 0; k++)
            if (Corner(Ids[(k + n - 1) % n], Ids[k], Ids[(k + 1) % n]) &&
                !Corner(Ids[k], Ids[(k + 1) % n], Ids[(k + 2) % n]))
                Ear = k;
        for (int k = 0; k < n && Ear < 0; k++)
            if (Corner(Ids[(k + n - 1) % n], Ids[k], Ids[(k + 1) % n]))
                Ear = k;
        if (Ear < 0)
            return false;
        if (!Tri.Add(Ids[(Ear + n - 1) % n]) || !Tri.Add(Ids[Ear]) || !Tri.Add(Ids[(Ear + 1) % n]))
            return false;
        for (int k = Ear; k + 1 < n; k++)
            Ids[k] = Ids[k + 1];
        Ids.Num--;
    }
    if (Ids.Num < 3 || !Corner(Ids[0], Ids[1], Ids[2]))
        return false;
    return Tri.Add(Ids[0]) && Tri.Add(Ids[1]) && Tri.Add(Ids[2]);
}

// The most corners a piece cut again where its places moved may have (CutMovedIntoTriangles): its lists live on the
// thread's stack and its time grows with the corners' cube; a piece with more keeps the cut it had.
static const int MostMoved = 64;

// Piece Ids (Num corners, in its ring's order; P each one's place now) into Num - 2 triangles of Out, three corners
// each, facing N's way: an ear at a time, of the corners turning N's way with none of the others in their triangle and
// no height within OnLine the best-shaped - its least height over its longest side the largest. Where a ball's corner
// pushes into a wedge of the piece it is convex no more, and still has its ears. False where it has none left: the moves
// fold the piece.
inline bool CutMovedIntoTriangles(const FV3* P, const int* Ids, int Num, const FV3& N, int* Out)
{
    if (Num < 3 || Num > MostMoved)
        return false;
    int Left[MostMoved];
    for (int c = 0; c < Num; c++)
        Left[c] = c;
    int  n    = Num;
    int* Next = Out;
    // Triangle a, b, c's shape, squared, as CutIntoTriangles takes it; -1 where it turns against N or has a height
    // within OnLine.
    auto Shape = [&](int a, int b, int c) {
        const FV3 X = RdxSphere::Cross(P[b] - P[a], P[c] - P[a]);
        if (RdxSphere::Dot(X, N) <= 0.0)
            return -1.0;
        const FV3    Ab = P[b] - P[a], Bc = P[c] - P[b], Ca = P[a] - P[c];
        const double Long = MaxOf(RdxSphere::Dot(Ab, Ab), MaxOf(RdxSphere::Dot(Bc, Bc), RdxSphere::Dot(Ca, Ca)));
        const double A2   = RdxSphere::Dot(X, X);
        if (A2 <= OnLine * OnLine * Long)
            return -1.0;
        return A2 / (Long * Long);
    };
    // Whether corner v lies in triangle a, b, c (N's way round), its sides too: a corner on the side the cut makes would
    // be left on no triangle's corner.
    auto Inside = [&](int v, int a, int b, int c) {
        return RdxSphere::Dot(RdxSphere::Cross(P[b] - P[a], P[v] - P[a]), N) >= 0.0 &&
               RdxSphere::Dot(RdxSphere::Cross(P[c] - P[b], P[v] - P[b]), N) >= 0.0 &&
               RdxSphere::Dot(RdxSphere::Cross(P[a] - P[c], P[v] - P[c]), N) >= 0.0;
    };
    while (n > 3)
    {
        int    Ear  = -1;
        double Best = -1.0;
        for (int k = 0; k < n; k++)
        {
            const int    a = Left[(k + n - 1) % n], b = Left[k], c = Left[(k + 1) % n];
            const double S = Shape(a, b, c);
            if (S <= Best)
                continue;
            bool bClear = true;
            for (int j = 0; j < n && bClear; j++)
            {
                const int v = Left[j];
                bClear = v == a || v == b || v == c || !Inside(v, a, b, c);
            }
            if (bClear)
            {
                Ear  = k;
                Best = S;
            }
        }
        if (Ear < 0)
            return false;
        *Next++ = Ids[Left[(Ear + n - 1) % n]];
        *Next++ = Ids[Left[Ear]];
        *Next++ = Ids[Left[(Ear + 1) % n]];
        for (int k = Ear; k + 1 < n; k++)
            Left[k] = Left[k + 1];
        n--;
    }
    if (Shape(Left[0], Left[1], Left[2]) < 0.0)
        return false;
    *Next++ = Ids[Left[0]];
    *Next++ = Ids[Left[1]];
    *Next++ = Ids[Left[2]];
    return true;
}

// A piece's ring (Ids, Corners places, in its triangles' way round) back from its Own triangles (Tri, three places
// each): its sides are the triangles' sides none of the others has the other way. False where they make no one ring of
// Corners places.
inline bool RingOfTriangles(const int* Tri, int Own, int* Ids, int Corners)
{
    if (Own < 1 || Own > MostMoved - 2 || Corners != Own + 2)
        return false;
    int From[3 * MostMoved], To[3 * MostMoved], Sides = 0;
    for (int i = 0; i < Own; i++)
        for (int k = 0; k < 3; k++)
        {
            const int A = Tri[i * 3 + k], B = Tri[i * 3 + (k + 1) % 3];
            bool      bInner = false;
            for (int j = 0; j < Own && !bInner; j++)
                for (int m = 0; m < 3 && !bInner; m++)
                    bInner = Tri[j * 3 + m] == B && Tri[j * 3 + (m + 1) % 3] == A;
            if (bInner)
                continue;
            if (Sides >= Corners)
                return false;
            From[Sides] = A;
            To[Sides]   = B;
            Sides++;
        }
    if (Sides != Corners)
        return false;
    // From the first triangle's first corner, where the cut began the ring.
    int First = 0;
    while (First < Sides && From[First] != Tri[0])
        First++;
    if (First >= Sides)
        First = 0;
    Ids[0]  = From[First];
    int Now = To[First];
    for (int n = 1; n < Corners; n++)
    {
        int e = 0;
        while (e < Sides && From[e] != Now)
            e++;
        if (e >= Sides)
            return false;
        Ids[n] = Now;
        Now    = To[e];
    }
    return Now == Ids[0];
}

} // namespace BevelDetail

// The level's walls bevelled with Radius along their edges (BevelEdgesOf) into R. Each face a band reaches into - by
// the edges' faces, every face the bevel may cut of their planes there, not the edges' own alone - is cut along the
// bands' lines (BandSlices on each face), its ring given first every point a line puts on one of its sides, made once
// for the side and the same for every face on it; the cut pieces go into triangles; every place is then moved once,
// from where the rounding drew it, by every band it lies in (RdxBevel::Round) - a box's corners onto the ball; and the
// polygon is drawn from then on as a moved one: R's PolyFirst and PolyTris, its triangles in Tri - a still polygon's
// with a TriTurn of 0 (the band is too narrow for the relief to turn on), a moved one's pieces with their triangle's.
// A face on a side a point goes onto, or a still polygon's on a place that moves, is made again too. An edge one of
// whose points would move a face the bevel may not touch, or would find no worn solid under it, is left sharp, and the
// bevel made again without it. Angle the rounding's. False where there was no memory for it; R is then as the rounding
// left it.
inline bool Bevel(const FV3* Points, int NumPoints, const FPolyIn* Polys, int NumPolys, const FV3* MorePoints,
                  const FPolyIn* MorePolys, int NumMore, double Angle, double Radius, FResult& R, FBevelStats& S,
                  FBevelMade& M)
{
    using namespace BevelDetail;
    M = FBevelMade();
    R.BevelNormal.Free();
    double LapAt = R.Seconds ? R.Seconds() : 0.0;
    auto   Lap   = [&](int Part) {
        if (!R.Seconds)
            return;
        const double Now = R.Seconds();
        M.PartMs[Part] += (Now - LapAt) * 1000.0;
        LapAt = Now;
    };
    FBevelPrep Prep;
    if (!BevelPrepOf(Polys, NumPolys, Angle, Radius, R, Prep))
        return false;
    Lap(0);
    if (RdxSphere::Abandoned())
        return false;
    TList<RdxBevel::FEdge> Edges;
    if (!BevelEdgesOf(Polys, Angle, Radius, R, Prep, Edges, S))
        return false;
    Lap(1);
    if (RdxSphere::Abandoned())
        return false;
    if (Edges.Num == 0)
        return true;
    if (Edges.Num * 2 * BandSlices >= (1 << LineBits))
        return false;
    const int            Places0  = R.Orig.Num;
    const int            NumFaces = Prep.Owner.Num;
    TList<unsigned char> Still, MustTake;
    if (!Still.Resize(NumFaces) || !MustTake.Resize(NumFaces))
        return false;
    // The faces the bevel may cut.
    for (int f = 0; f < NumFaces; f++)
    {
        Still[f]    = Prep.Still[f] ? 1 : 0;
        MustTake[f] = 0;
    }

    // Every face's sides (Prep's: place U, place V, the face), sorted - the faces on a side, either way round -, and
    // every face's places with their face, sorted - the faces on a place.
    const TList<long long>& SideKeys = Prep.SideKeys;
    TList<long long>        PlaceKeys;
    for (int f = 0; f < NumFaces; f++)
    {
        const int C = CornersOf(Prep, R, f);
        for (int i = 0; i < C; i++)
            if (!PlaceKeys.Add(((long long)CornerOf(Prep, R, f, i) << FaceBits) | f))
                return false;
    }
    // The sorts' room - let go after each of the large ones, which a making's later parts would hold it through.
    TList<long long> SortRoom;
    SortOnThreads(PlaceKeys, R.Workers, SortRoom);
    const long long FaceMask = (1LL << FaceBits) - 1;
    // The faces with the side from U to V, and those on place U, into Out (not cleared).
    auto FacesOnSide = [&](long long U, long long V, TList<int>& Out) {
        const long long Top = (U << PlaceBits) | V;
        for (int a = FirstAbove(SideKeys, Top, FaceBits); a < SideKeys.Num && (SideKeys[a] >> FaceBits) == Top; a++)
            if (!Out.Add((int)(SideKeys[a] & FaceMask)))
                return false;
        return true;
    };
    auto FacesOnPlace = [&](long long U, TList<int>& Out) {
        for (int a = FirstAbove(PlaceKeys, U, FaceBits); a < PlaceKeys.Num && (PlaceKeys[a] >> FaceBits) == U; a++)
            if (!Out.Add((int)(PlaceKeys[a] & FaceMask)))
                return false;
        return true;
    };

    TList<unsigned char> Drop;
    TList<double>        Width;
    if (!Drop.Resize(Edges.Num) || !Width.Resize(Edges.Num))
        return false;
    for (int e = 0; e < Edges.Num; e++)
    {
        Drop[e]  = 0;
        Width[e] = RdxBevel::BandWidth(Edges[e], Radius);
    }
    auto BandBox = [&](int e, FV3& Lo, FV3& Hi) {
        const RdxBevel::FEdge& E = Edges[e];
        const FV3    A = E.From - E.Way * Width[e], B = E.From + E.Way * (E.Length + Width[e]);
        const double W = Width[e] + RdxBevel::OnFace;
        Lo             = RdxSphere::V3(MinOf(A.X, B.X) - W, MinOf(A.Y, B.Y) - W, MinOf(A.Z, B.Z) - W);
        Hi             = RdxSphere::V3(MaxOf(A.X, B.X) + W, MaxOf(A.Y, B.Y) + W, MaxOf(A.Z, B.Z) + W);
    };
    // A band's line by its number: edge e's (LineEdge) face f (0 its A, 1 its B) at the band's width (slice 0) or a
    // step of it nearer the edge for each slice after (BandSlices).
    auto LineEdge = [](int Id) { return Id / (2 * BandSlices); };
    auto LineInto = [&](int Id) {
        return ((Id / BandSlices) & 1) ? Edges[LineEdge(Id)].IntoB : Edges[LineEdge(Id)].IntoA;
    };
    auto LineAt   = [&](int Id) {
        return Width[LineEdge(Id)] * (double)(BandSlices - Id % BandSlices) / (double)BandSlices;
    };
    auto LineSide = [&](int Id, const FV3& P) {
        return RdxSphere::Dot(P - Edges[LineEdge(Id)].From, LineInto(Id)) - LineAt(Id);
    };

    // The cells StillCell wide a band reaches into (Banded), sorted, each once: no other is ever looked in - the faces the
    // bevel may cut and the sides of the polygons that are no walls are found by them alone.
    TList<long long> Banded;
    for (int e = 0; e < Edges.Num; e++)
    {
        FV3 Lo, Hi;
        BandBox(e, Lo, Hi);
        bool bOk = true;
        EachCell(Lo, Hi, StillCell, 1 << 16, [&](long long K) { return bOk = Banded.Add(K); });
        if (!bOk)
            return false;
    }
    SortOnThreads(Banded, R.Workers, SortRoom);
    {
        int Kept = 0;
        for (int i = 0; i < Banded.Num; i++)
            if (Kept == 0 || Banded[Kept - 1] != Banded[i])
                Banded[Kept++] = Banded[i];
        Banded.Num = Kept;
    }
    auto IsBanded = [&](long long K) {
        int a = 0, b = Banded.Num;
        while (a < b)
        {
            const int m = (a + b) / 2;
            if (Banded[m] < K)
                a = m + 1;
            else
                b = m;
        }
        return a < Banded.Num && Banded[a] == K;
    };

    // The faces the bevel may cut by the banded cells their boxes reach into - counted first, for the table's room.
    Detail::FCells FaceGrid;
    {
        auto FaceBox = [&](int f, FV3& Lo, FV3& Hi) {
            const int C = CornersOf(Prep, R, f);
            Lo = Hi = R.Moved[CornerOf(Prep, R, f, 0)];
            for (int i = 1; i < C; i++)
            {
                const FV3& Q = R.Moved[CornerOf(Prep, R, f, i)];
                Lo           = RdxSphere::V3(MinOf(Lo.X, Q.X), MinOf(Lo.Y, Q.Y), MinOf(Lo.Z, Q.Z));
                Hi           = RdxSphere::V3(MaxOf(Hi.X, Q.X), MaxOf(Hi.Y, Q.Y), MaxOf(Hi.Z, Q.Z));
            }
        };
        long long Entries = 0;
        for (int f = 0; f < NumFaces; f++)
            if (Still[f] == 1)
            {
                FV3 Lo, Hi;
                FaceBox(f, Lo, Hi);
                EachCell(Lo, Hi, StillCell, 1 << 16, [&](long long K) {
                    Entries += IsBanded(K) ? 1 : 0;
                    return true;
                });
            }
        if (Entries >= (1LL << 30) || !FaceGrid.InitExact((int)Entries))
            return false;
        for (int f = 0; f < NumFaces; f++)
        {
            if (Still[f] != 1)
                continue;
            FV3 Lo, Hi;
            FaceBox(f, Lo, Hi);
            // A face over too many cells is found by none of the bands, and left as it is.
            bool bOk = true;
            EachCell(Lo, Hi, StillCell, 1 << 16, [&](long long K) { return bOk = !IsBanded(K) || FaceGrid.Add(K, f); });
            if (!bOk)
                return false;
        }
    }

    // The sides of the polygons that are no walls - invisible, portals, mirrors, the sky's, the movers' the level
    // holds: no ring of theirs - from the level's own points, and of MorePolys (MorePoints theirs: the movers' own
    // polygons, where they stand when the level is rounded), by cells. A place that moves must not lie on one: a
    // mirror, the sky or a door would stay behind it - a secret door's outline drawn round. Of their cells, EdgeCell
    // wide, those in a banded cell alone: a place that moves lies in a band, and the cell it is looked for in (OnOther) in
    // a banded one of that band's. Counted first, for the table's room.
    Detail::FCells Others;
    TList<FV3>     OtherEnds;
    int            MoversFrom = -1;   // the first of the sides that are the movers' (MorePolys)
    {
        long long Entries = 0, Sides = 0;
        for (int Pass = 0; Pass < 2; Pass++)
        {
            if (Pass == 1 && (Entries >= (1LL << 30) || Sides >= (1LL << 29) || !Others.InitExact((int)Entries) ||
                              !OtherEnds.ReserveExact((int)Sides * 2)))
                return false;
            for (int q = 0; q < NumPolys + NumMore; q++)
            {
                const bool     bMore = q >= NumPolys;
                if (Pass == 1 && bMore && MoversFrom < 0)
                    MoversFrom = OtherEnds.Num / 2;
                const FPolyIn& Q     = bMore ? MorePolys[q - NumPolys] : Polys[q];
                const FV3*     At    = bMore ? MorePoints : Points;
                if ((Q.Use & RdxSphere::Use_Wall) || Q.Count < 3 || Q.First < 0 ||
                    (!bMore && Q.First + Q.Count > NumPoints))
                    continue;
                for (int k = 0; k < Q.Count; k++)
                {
                    const FV3& A = At[Q.First + k];
                    const FV3& B = At[Q.First + (k + 1) % Q.Count];
                    const int  s = OtherEnds.Num / 2;
                    if (Pass == 0)
                        Sides++;
                    else if (!OtherEnds.Add(A) || !OtherEnds.Add(B))
                        return false;
                    const FV3 Lo  = RdxSphere::V3(MinOf(A.X, B.X) - OnEdge, MinOf(A.Y, B.Y) - OnEdge,
                                                  MinOf(A.Z, B.Z) - OnEdge);
                    const FV3 Hi  = RdxSphere::V3(MaxOf(A.X, B.X) + OnEdge, MaxOf(A.Y, B.Y) + OnEdge,
                                                  MaxOf(A.Z, B.Z) + OnEdge);
                    bool      bOk = true;
                    EachCell(Lo, Hi, EdgeCell, 1 << 16, [&](long long K) {
                        if (!IsBanded(CellFourWide(K)))
                            return true;
                        if (Pass == 0)
                        {
                            Entries++;
                            return true;
                        }
                        return bOk = Others.Add(K, s);
                    });
                    if (!bOk)
                        return false;
                }
            }
        }
    }
    Banded.Free();
    // 0: P lies on no such side; 1: on one of the level's polygons'; 2: on one of the movers'.
    auto OnOther = [&](const FV3& P) {
        const long long K = Detail::CellKey((long long)floor(P.X / EdgeCell), (long long)floor(P.Y / EdgeCell),
                                            (long long)floor(P.Z / EdgeCell));
        int Kind = 0;
        for (int g = Others.Head[Others.Bucket(K)]; g >= 0 && Kind < 2; g = Others.Next[g])
        {
            const int s = Others.Item[g];
            if (Others.Key[g] == K && StretchDistance(P, OtherEnds[s * 2], OtherEnds[s * 2 + 1]) <= OnEdge)
                Kind = (MoversFrom >= 0 && s >= MoversFrom) ? 2 : 1;
        }
        return Kind;
    };

    // Each thread's own record of the faces it has looked at for an edge's face, and its own pairs (below): made for
    // each making's search and let go after it - the making's later parts hold the most.
    const int        Threads = (R.Workers < 1) ? 1 : ((R.Workers > RdxSphere::WorkersMost) ? RdxSphere::WorkersMost
                                                                                          : R.Workers);
    TList<int>       Seens[RdxSphere::WorkersMost];
    TList<long long> PairsOf[RdxSphere::WorkersMost];
    // The least cosine between two faces of one surface: they meet at the rounding's angle or wider (BevelEdgesOf).
    const double     OneSurface = cos(RdxSphere::Pi - Angle * RdxSphere::Pi / 180.0);
    Lap(2);
    if (RdxSphere::Abandoned())
        return false;
    for (M.Rounds = 1; M.Rounds <= MostRounds; M.Rounds++)
    {
        int Fresh = 0;   // the edges this making left sharp and the faces it brought in for the next
        // The edges' bands by the cells they reach into, for the places' moves - counted first, for the table's room.
        Detail::FCells EdgeGrid;
        long long      Cells = 0;
        for (int e = 0; e < Edges.Num; e++)
            if (!Drop[e])
            {
                FV3 Lo, Hi;
                BandBox(e, Lo, Hi);
                Cells += CellsOf(Lo, Hi, EdgeCell, 1 << 16);
            }
        if (Cells >= (1LL << 30) || !EdgeGrid.InitExact((int)Cells))
            return false;
        for (int e = 0; e < Edges.Num; e++)
        {
            if (Drop[e])
                continue;
            FV3 Lo, Hi;
            BandBox(e, Lo, Hi);
            bool bOk = true;
            if (!EachCell(Lo, Hi, EdgeCell, 1 << 16, [&](long long K) { return bOk = EdgeGrid.Add(K, e); }))
            {
                if (!bOk)
                    return false;
                Drop[e] = 1;   // a band across the whole level: left sharp
            }
        }

        // Each face of an edge's face's plane a band reaches into, with the band's lines: (face, line), sorted - the
        // edges over the rounding's threads, each into its thread's own list; sorted, the lists are one.
        // And a moved wall's triangle off that plane - a facet of the surface the rounding curved there, of one surface
        // with the edge's face (turned less than the rounding's angle) - that touches the band at the edge's end and
        // reaches past it: where two short edges of a curve meet, the facets between their faces touch the band at
        // that corner alone. The lines are planes square to the face's way in, so they cut such a facet where they cut
        // the faces beside it, at the same points on the sides they share: the corner's move stays within the band, as
        // along the rest of the curve - left uncut, the facet would tilt whole with it, or hold it back out of the
        // round.
        TList<long long> Pairs;
        for (int w = 0; w < Threads; w++)
        {
            if (!Seens[w].Resize(NumFaces))
                return false;
            for (int f = 0; f < NumFaces; f++)
                Seens[w][f] = -1;
            PairsOf[w].Clear();
        }
        std::atomic<bool> NoRoom(false);
        RdxSphere::OnThreads(Edges.Num, 16, Threads, [&](int e, int Worker) {
            if (Drop[e] || NoRoom)
                return;
            TList<int>&            Seen = Seens[Worker];
            TList<long long>&      Out  = PairsOf[Worker];
            const RdxBevel::FEdge& E    = Edges[e];
            FV3                    Lo, Hi;
            BandBox(e, Lo, Hi);
            for (int s = 0; s < 2; s++)
            {
                const FV3 N    = s ? E.NormalB : E.NormalA;
                const FV3 Into = s ? E.IntoB : E.IntoA;
                bool      bOk  = true;
                EachCell(Lo, Hi, StillCell, 1 << 16, [&](long long K) {
                    for (int g = FaceGrid.Head[FaceGrid.Bucket(K)]; g >= 0 && bOk; g = FaceGrid.Next[g])
                    {
                        if (FaceGrid.Key[g] != K)
                            continue;
                        const int f = FaceGrid.Item[g];
                        if (Seen[f] == e * 2 + s)
                            continue;
                        Seen[f] = e * 2 + s;
                        const int  C        = CornersOf(Prep, R, f);
                        const bool bOnPlane = RdxSphere::Dot(Prep.Normal[f], N) >= SameWay &&
                                              fabs(RdxSphere::Dot(R.Moved[CornerOf(Prep, R, f, 0)] - E.From, N)) <= OnPlane;
                        bool       bFacet   = false;
                        if (!bOnPlane && f >= Prep.RingFaces && RdxSphere::Dot(Prep.Normal[f], N) > OneSurface)
                        {
                            // One corner the edge's end, every other further in than the band's width: the lines cross
                            // it near that corner alone - not along a facet lying beside the edge, which they would cut
                            // into slivers its end's move turns over.
                            int AtEnd = 0, Past = 0, End = -1;
                            for (int i = 0; i < C; i++)
                            {
                                const FV3    Off   = R.Moved[CornerOf(Prep, R, f, i)] - E.From;
                                const double Along = RdxSphere::Dot(Off, E.Way);
                                if (RdxSphere::Length(Off - E.Way * Along) <= OnLine &&
                                    (fabs(Along) <= OnLine || fabs(Along - E.Length) <= OnLine))
                                {
                                    AtEnd++;
                                    End = i;
                                }
                                else if (RdxSphere::Dot(Off, Into) >= Width[e])
                                    Past++;
                            }
                            bFacet = AtEnd == 1 && AtEnd + Past == C;
                            // A needle of a facet, whose two sides from the end the nearest line crosses within OnLine
                            // of each other, is left uncut: the cut would put one point on both, and leave the edge
                            // sharp. Thin as it is, it tilts with the corner unseen.
                            if (bFacet)
                            {
                                const FV3    E0 = R.Moved[CornerOf(Prep, R, f, End)];
                                const FV3    Q1 = R.Moved[CornerOf(Prep, R, f, (End + 1) % C)];
                                const FV3    Q2 = R.Moved[CornerOf(Prep, R, f, (End + C - 1) % C)];
                                const double I0 = RdxSphere::Dot(E0 - E.From, Into);
                                const double D  = Width[e] / BandSlices - I0;
                                const double I1 = RdxSphere::Dot(Q1 - E0, Into), I2 = RdxSphere::Dot(Q2 - E0, Into);
                                bFacet = RdxSphere::Length((Q1 - E0) * (D / I1) - (Q2 - E0) * (D / I2)) > OnLine;
                            }
                        }
#ifdef LEVELBEVEL_TRACE
                        bFacet = bFacet && !LevelBevelFacetsOff();
                        if (!bOnPlane && f >= Prep.RingFaces)
                            for (int i = 0; i < C; i++)
                                if (CornerOf(Prep, R, f, i) == LevelBevelTracePlace())
                                {
                                    printf("  trace %d: facet %d by edge %d side %d (from %.3f %.3f %.3f way %.3f %.3f %.3f "
                                           "length %.3f, band %.3f): normal's cosine %.3f, corners:",
                                           LevelBevelTracePlace(), f, e, s, E.From.X, E.From.Y, E.From.Z, E.Way.X, E.Way.Y,
                                           E.Way.Z, E.Length, Width[e], RdxSphere::Dot(Prep.Normal[f], N));
                                    for (int j = 0; j < C; j++)
                                    {
                                        const FV3 Off = R.Moved[CornerOf(Prep, R, f, j)] - E.From;
                                        printf(" (along %.3f, off the line %.3f, in %.3f)", RdxSphere::Dot(Off, E.Way),
                                               RdxSphere::Length(Off - E.Way * RdxSphere::Dot(Off, E.Way)),
                                               RdxSphere::Dot(Off, Into));
                                    }
                                    printf(" - %s\n", bFacet ? "cut" : "not cut");
                                }
#endif
                        if (!bOnPlane && !bFacet)
                            continue;
                        double A0 = 1e300, A1 = -1e300, I0 = 1e300, I1 = -1e300;
                        for (int i = 0; i < C; i++)
                        {
                            const FV3 Off = R.Moved[CornerOf(Prep, R, f, i)] - E.From;
                            A0            = MinOf(A0, RdxSphere::Dot(Off, E.Way));
                            A1            = MaxOf(A1, RdxSphere::Dot(Off, E.Way));
                            I0            = MinOf(I0, RdxSphere::Dot(Off, Into));
                            I1            = MaxOf(I1, RdxSphere::Dot(Off, Into));
                        }
                        if (A1 < -Width[e] || A0 > E.Length + Width[e] || I1 <= OnLine || I0 >= Width[e])
                            continue;
                        for (int k = 0; k < BandSlices && bOk; k++)
                            bOk = Out.Add(((long long)f << LineBits) | (e * 2 * BandSlices + s * BandSlices + k));
                    }
                    return bOk;
                });
                if (!bOk)
                    NoRoom = true;
            }
        });
        if (NoRoom)
            return false;
        int PairsAll = 0;
        for (int w = 0; w < Threads; w++)
            PairsAll += PairsOf[w].Num;
        if (!Pairs.ReserveExact(PairsAll))
            return false;
        for (int w = 0; w < Threads; w++)
        {
            for (int k = 0; k < PairsOf[w].Num; k++)
                if (!Pairs.Add(PairsOf[w][k]))
                    return false;
            Seens[w].Free();
            PairsOf[w].Free();
        }
        SortOnThreads(Pairs, R.Workers, SortRoom);
        SortRoom.Free();

        // The places the lines put on stretches between two places, each made once (Made: the stretch to its places);
        // and of those on the rings' own sides, the side, the share along it from its lower place and the line
        // (OnSides).
        // A new place is where the rounding draws its stretch there (New), and where it lies on its polygon by the same
        // share of the stretch where the stretch's ends lay on it (NewOrig): the texture's place - on a still polygon
        // the same.
        Detail::FCells Made;
        TList<FV3>     New, NewOrig;
        struct FOnSide
        {
            long long Side;
            double    T;
            int       Place, Line;
        };
        TList<FOnSide> OnSides;
        Detail::FCells NewAt;   // the new places by small cells
        if (!Made.Init(Pairs.Num + 64) || !NewAt.Init(Pairs.Num + 64))   // a point or so a pair: the tables grow past it
            return false;
        // Which small cells may hold a new place, a bit each by a hash of the cell's key - a bit clear: none of them
        // does. The new places are looked for in the 27 cells around a point, most of them empty, and a bit is looked at
        // in a table far smaller than NewAt's: NewAt's list is walked only behind a set bit, the same places found in the
        // same order. Every place put into NewAt goes through AddNewAt.
        TList<unsigned int> NewSeen;
        if (!NewSeen.Resize(1 << (NewSeenBits - 5)))
            return false;
        for (int i = 0; i < NewSeen.Num; i++)
            NewSeen[i] = 0;
        auto NewSeenSlot = [](long long K) {
            return (unsigned int)(((unsigned long long)K * 0x9E3779B97F4A7C15ull) >> (64 - NewSeenBits));
        };
        auto NewMayHold = [&](long long K) {
            const unsigned int S = NewSeenSlot(K);
            return (NewSeen[(int)(S >> 5)] & (1u << (S & 31))) != 0;
        };
        auto AddNewAt = [&](long long K, int w) {
            const unsigned int S = NewSeenSlot(K);
            NewSeen[(int)(S >> 5)] |= 1u << (S & 31);
            return NewAt.Add(K, w);
        };
        auto AtPlace   = [&](int w) { return (w < Places0) ? R.Moved[w] : New[w - Places0]; };
        auto OrigPlace = [&](int w) { return (w < Places0) ? R.Orig[w] : NewOrig[w - Places0]; };
        auto NewCell = [](double V) { return (long long)floor(V / NewCellSize); };
        // A new place within OnLine of X, or -1.
        auto NewNear = [&](const FV3& X) {
            const long long Cx = NewCell(X.X), Cy = NewCell(X.Y), Cz = NewCell(X.Z);
            for (long long z = Cz - 1; z <= Cz + 1; z++)
                for (long long y = Cy - 1; y <= Cy + 1; y++)
                    for (long long x = Cx - 1; x <= Cx + 1; x++)
                    {
                        const long long K = Detail::CellKey(x, y, z);
                        if (!NewMayHold(K))
                            continue;
                        for (int g = NewAt.Head[NewAt.Bucket(K)]; g >= 0; g = NewAt.Next[g])
                            if (NewAt.Key[g] == K && RdxSphere::Length(New[NewAt.Item[g] - Places0] - X) <= OnLine)
                                return NewAt.Item[g];
                    }
            return -1;
        };
        // The place line Id puts on the stretch from place I to J that it goes across - a new place already within
        // OnLine of it being the same, whichever stretch made it: two edges' lines may fall together on a narrow face,
        // and a line may cross a piece's inner side just where another put a point on the ring.
        auto CrossAt = [&](int I, int J, int Id, bool bRingSide) {
            const long long Lo = (I < J) ? I : J, Hi = (I < J) ? J : I;
            const long long Key = (Lo << PlaceBits) | Hi;
            const FV3       A = AtPlace((int)Lo), B = AtPlace((int)Hi);
            const double    Dl = LineSide(Id, A), Dh = LineSide(Id, B);
            const double    T  = Dl / (Dl - Dh);
            const FV3       X  = A + (B - A) * T;
            for (int g = Made.Head[Made.Bucket(Key)]; g >= 0; g = Made.Next[g])
                if (Made.Key[g] == Key && RdxSphere::Length(AtPlace(Made.Item[g]) - X) <= OnLine)
                    return Made.Item[g];
            int w = NewNear(X);
            if (w < 0)
            {
                w = Places0 + New.Num;
                // A place past what a side's key holds (PlaceBits): the level too large for the bevel, which leaves it
                // as the rounding made it.
                if (w >= (1 << PlaceBits))
                    return -1;
                const FV3 Oa = OrigPlace((int)Lo), Ob = OrigPlace((int)Hi);
                if (!New.Add(X) || !NewOrig.Add(Oa + (Ob - Oa) * T) ||
                    !AddNewAt(Detail::CellKey(NewCell(X.X), NewCell(X.Y), NewCell(X.Z)), w))
                    return -1;
            }
            if (!Made.Add(Key, w))
                return -1;
            if (bRingSide)
            {
                const FOnSide On = { Key, T, w, Id };
                if (!OnSides.Add(On))
                    return -1;
            }
            return w;
        };
        // Which of a face's sides each of its lines goes across, worked out on the threads - a run of Pairs is one
        // face's, and a face's corners' sides of its lines are worked out once, a corner being the end of two of its
        // sides; then the places made run by run, side by side and line by line, as one thread would make them: the
        // same places in the same order. A crossing is kept as its side times the run's lines, plus its line.
        TList<int> RunAt;
        for (int a = 0; a < Pairs.Num;)
        {
            const int f = (int)(Pairs[a] >> LineBits);
            int       b = a;
            while (b < Pairs.Num && (int)(Pairs[b] >> LineBits) == f)
                b++;
            if (!RunAt.Add(a))
                return false;
            a = b;
        }
        const int FaceRuns = RunAt.Num;
        if (!RunAt.Add(Pairs.Num))
            return false;
        TList<int> RunWorker, RunFirst, RunCount;
        if (!RunWorker.Resize(FaceRuns) || !RunFirst.Resize(FaceRuns) || !RunCount.Resize(FaceRuns))
            return false;
        TList<int>        CrossOf[RdxSphere::WorkersMost];
        TList<int>        FaceCorner[RdxSphere::WorkersMost];
        TList<double>     FaceSide[RdxSphere::WorkersMost];
        std::atomic<bool> NoCrossRoom(false);
        RdxSphere::OnThreads(FaceRuns, 16, Threads, [&](int r, int Worker) {
            if (NoCrossRoom)
                return;
            const int      a = RunAt[r], Lines = RunAt[r + 1] - a, f = (int)(Pairs[a] >> LineBits);
            const int      C      = CornersOf(Prep, R, f);
            TList<int>&    Corner = FaceCorner[Worker];
            TList<double>& Side   = FaceSide[Worker];
            TList<int>&    Out    = CrossOf[Worker];
            if (!Corner.Resize(C) || !Side.Resize(C * Lines))
            {
                NoCrossRoom = true;
                return;
            }
            for (int i = 0; i < C; i++)
                Corner[i] = CornerOf(Prep, R, f, i);
            for (int k = 0; k < Lines; k++)
            {
                const int Id = (int)(Pairs[a + k] & ((1 << LineBits) - 1));
                for (int i = 0; i < C; i++)
                    Side[k * C + i] = LineSide(Id, R.Moved[Corner[i]]);
            }
            RunWorker[r] = Worker;
            RunFirst[r]  = Out.Num;
            for (int i = 0; i < C; i++)
            {
                if (Corner[i] == Corner[(i + 1) % C])
                    continue;
                for (int k = 0; k < Lines; k++)
                {
                    const double Du = Side[k * C + i], Dv = Side[k * C + (i + 1) % C];
                    if (((Du > OnLine && Dv < -OnLine) || (Du < -OnLine && Dv > OnLine)) && !Out.Add(i * Lines + k))
                    {
                        NoCrossRoom = true;
                        return;
                    }
                }
            }
            RunCount[r] = Out.Num - RunFirst[r];
        });
        for (int w = 0; w < RdxSphere::WorkersMost; w++)
        {
            FaceCorner[w].Free();
            FaceSide[w].Free();
        }
        if (NoCrossRoom)
            return false;
        for (int r = 0; r < FaceRuns; r++)
        {
            const int         a = RunAt[r], Lines = RunAt[r + 1] - a, f = (int)(Pairs[a] >> LineBits);
            const int         C   = CornersOf(Prep, R, f);
            const TList<int>& Out = CrossOf[RunWorker[r]];
            for (int e = RunFirst[r]; e < RunFirst[r] + RunCount[r]; e++)
            {
                const int i = Out[e] / Lines, Id = (int)(Pairs[a + Out[e] % Lines] & ((1 << LineBits) - 1));
                if (CrossAt(CornerOf(Prep, R, f, i), CornerOf(Prep, R, f, (i + 1) % C), Id, true) < 0)
                    return false;
            }
        }

        Lap(3);
        if (RdxSphere::Abandoned())
            return false;
        // Where each place goes (MoveOf: 1 it stays, 2 it moves to MoveAt, -1 no worn solid under it), worked out the
        // first time it is asked for, by the bands of the edges not left sharp then that it lies in, in the edges' order
        // - the same place goes the same way whichever polygon asks.
        // A place's move in a slot of its own, given when it is first asked for (MoveSlot, -1 before): where it goes
        // (MoveAt) and for one that moves the round's normal there (MoveNrm, PackedNormalOf) - the places near a band
        // alone take room, not every place of the level.
        TList<int>           MoveSlot;
        TList<FV3>           MoveAt;
        TList<unsigned int>  MoveNrm;
        TList<signed char>   MoveState;
        TList<signed char>   OtherOf;   // for a place that moves, OnOther's answer there
        TList<unsigned char> Ahead;     // worked out ahead (MovesOf), not yet asked for
        auto                 Grow = [&]() {
            const int All = Places0 + New.Num, Had = MoveState.Num;
            if (Had >= All)
                return true;
            if (!MoveSlot.Resize(All) || !MoveState.Resize(All) || !OtherOf.Resize(All) || !Ahead.Resize(All))
                return false;
            for (int w = Had; w < All; w++)
            {
                MoveSlot[w]  = -1;
                MoveState[w] = 0;
                Ahead[w]     = 0;
            }
            return true;
        };
        // Place w's slot, given now where it has none - on the caller's thread alone, the moves' threads not running.
        auto Slotted = [&](int w) {
            if (MoveSlot[w] >= 0)
                return true;
            MoveSlot[w] = MoveAt.Num;
            return MoveAt.Add(RdxSphere::V3(0.0, 0.0, 0.0)) && MoveNrm.Add(0u);
        };
        // The edges whose bands a place lies in (NearOf) - each thread its own room for them.
        struct FNear
        {
            RdxBevel::FEdge Edges[MostAtPoint];
            int             Id[MostAtPoint];
            int             Count;
            TList<int>      On;   // the faces on the place (MoveInto)
        };
        FNear Nears[RdxSphere::WorkersMost];
        auto  NearOf = [&](const FV3& P, FNear& N) {
            N.Count           = 0;
            const long long K = Detail::CellKey((long long)floor(P.X / EdgeCell), (long long)floor(P.Y / EdgeCell),
                                                (long long)floor(P.Z / EdgeCell));
            for (int g = EdgeGrid.Head[EdgeGrid.Bucket(K)]; g >= 0; g = EdgeGrid.Next[g])
            {
                if (EdgeGrid.Key[g] != K)
                    continue;
                const int e = EdgeGrid.Item[g];
                if (Drop[e] || !RdxBevel::InBand(Edges[e], P, Radius))
                    continue;
                bool bHave = false;
                for (int k = 0; k < N.Count && !bHave; k++)
                    bHave = N.Id[k] == e;
                if (bHave || N.Count >= MostAtPoint)
                    continue;
                int k = N.Count++;
                while (k > 0 && N.Id[k - 1] > e)
                {
                    N.Id[k]    = N.Id[k - 1];
                    N.Edges[k] = N.Edges[k - 1];
                    k--;
                }
                N.Id[k]    = e;
                N.Edges[k] = Edges[e];
            }
        };
        // Whether one of the lines of the edges N names cuts face f (Pairs).
        auto CutBy = [&](int f, const FNear& N) {
            for (int a = FirstAbove(Pairs, f, LineBits); a < Pairs.Num && (int)(Pairs[a] >> LineBits) == f; a++)
            {
                const int e = LineEdge((int)(Pairs[a] & ((1LL << LineBits) - 1)));
                for (int k = 0; k < N.Count; k++)
                    if (N.Id[k] == e)
                        return true;
            }
            return false;
        };
        auto MoveInto = [&](int w, FNear& N) {
            const FV3 P = AtPlace(w);
            NearOf(P, N);
            FV3       To = P, Nrm = RdxSphere::V3(0.0, 0.0, 0.0);
            const int Planes = (N.Count > 0) ? RdxBevel::Round(N.Edges, N.Count, P, Radius, To, Nrm) : 0;
            // A still wall's ring on the place that no line of the edges moving it cuts, reaching further from it than the
            // round does: nothing keeps the move near the place on it, and the whole of its piece there would tilt with
            // the place - the move stays in its plane. Two such planes: along the line they meet in; more: the place
            // stays. A face all within the round's reach of the place tilts inside the round, as the round's own pieces
            // do. A moved wall's triangle keeps nothing: it is a facet of a surface the rounding curved already, a few
            // degrees off its neighbours, and it tilts with the place as the round's pieces do - held in the planes of
            // such facets, a corner where the bands pass by would stay where it was and stand out of the round.
            if (Planes > 0 && w < Places0 && RdxSphere::Length(To - P) > 0.0)
            {
                FV3 Keep[2];
                int Kept = 0;
                N.On.Clear();
                FacesOnPlace(w, N.On);
                for (int j = 0; j < N.On.Num && Kept < 3; j++)
                {
                    const int f = N.On[j];
                    if (CutBy(f, N))
                        continue;
                    bool bOldRule = false;
#ifdef LEVELBEVEL_TRACE
                    // The level check's A/B (LRC_BEVEL_FACETS_OFF): the rule before, a moved wall's triangle reaching
                    // sixteen radii held the move in its plane.
                    bOldRule = LevelBevelFacetsOff();
#endif
                    if (f >= Prep.RingFaces && !bOldRule)
                        continue;
                    double Far = 0.0;
                    for (int i = 0; i < CornersOf(Prep, R, f); i++)
                        Far = MaxOf(Far, RdxSphere::Length(AtPlace(CornerOf(Prep, R, f, i)) - P));
                    if (Far <= ((f >= Prep.RingFaces) ? 16.0 : TiltReach) * Radius)
                        continue;
                    const FV3 Nf    = Prep.Normal[f];
                    bool      bSame = false;
                    for (int k = 0; k < Kept && k < 2 && !bSame; k++)
                        bSame = fabs(RdxSphere::Dot(Keep[k], Nf)) >= SameWay;
#ifdef LEVELBEVEL_TRACE
                    if (w == LevelBevelTracePlace())
                        printf("  trace %d: face %d (normal %.3f %.3f %.3f, %d corners) reaches %.2f, no line of its "
                               "edges cuts it - %s\n", w, f, Nf.X, Nf.Y, Nf.Z, CornersOf(Prep, R, f), Far,
                               bSame ? "the same plane as one kept" : "kept");
#endif
                    if (bSame)
                        continue;
                    if (Kept < 2)
                        Keep[Kept] = Nf;
                    Kept++;
                }
                FV3 Go = To - P;
                if (Kept == 1)
                    Go = Go - Keep[0] * RdxSphere::Dot(Go, Keep[0]);
                else if (Kept == 2)
                {
                    const FV3    Line = RdxSphere::Cross(Keep[0], Keep[1]);
                    const double L    = RdxSphere::Length(Line);
                    Go                = (L > 1e-9) ? Line * (RdxSphere::Dot(Go, Line) / (L * L)) : RdxSphere::V3(0.0, 0.0, 0.0);
                }
                else if (Kept > 2)
                    Go = RdxSphere::V3(0.0, 0.0, 0.0);
#ifdef LEVELBEVEL_TRACE
                if (RdxSphere::Length(To - P) > 0.1 && RdxSphere::Length(Go) < 0.25 * RdxSphere::Length(To - P))
                {
                    LevelBevelKeptCounts()[0]++;
                    LevelBevelKeptCounts()[1] += (Kept > 2) ? 1 : 0;
                }
                if (w == LevelBevelTracePlace())
                    printf("  trace %d at %.3f %.3f %.3f: %d edges, %d planes, the round's move %.3f %.3f %.3f, %d faces "
                           "kept, the move left %.3f %.3f %.3f\n", w, P.X, P.Y, P.Z, N.Count, Planes, To.X - P.X,
                           To.Y - P.Y, To.Z - P.Z, Kept, Go.X, Go.Y, Go.Z);
#endif
                To = P + Go;
            }
#ifdef LEVELBEVEL_TRACE
            else if (w == LevelBevelTracePlace())
                printf("  trace %d at %.3f %.3f %.3f: %d edges, %d planes, the round's move %.3f %.3f %.3f\n", w, P.X, P.Y,
                       P.Z, N.Count, Planes, To.X - P.X, To.Y - P.Y, To.Z - P.Z);
#endif
            MoveAt[MoveSlot[w]]  = To;
            MoveState[w]         = (Planes < 0) ? -1 : ((RdxSphere::Length(To - P) > 0.0) ? 2 : 1);
            MoveNrm[MoveSlot[w]] = (MoveState[w] == 2) ? PackedNormalOf(Nrm) : 0u;
            OtherOf[w]       = (MoveState[w] == 2) ? (signed char)OnOther(P) : 0;
        };
        // An edge left sharp in this making already: a move worked out ahead, before it, is worked out again when it is
        // first asked for - as it would have been then.
        bool bAgain = false;
        bool bSlotRoom = true;   // false: no memory for a slot (MoveOf), the making given up
        // The edges' cells (EdgeCell wide, as EdgeGrid's) that an edge left sharp in this making reaches into, a bit
        // each by a hash of the cell's key. A place's move hangs on what is left sharp only through the edges of its
        // own cell (NearOf): where that cell's bit is clear, working a move out ahead of time again would come out the
        // same, and the one worked out is kept. A bit set by another cell's hash only works one out again.
        TList<unsigned int> DroppedNear;
        bool                bDroppedAll = false;   // an edge whose cells could not all be marked: every cell counts
        if (!DroppedNear.Resize(1 << (DroppedNearBits - 5)))
            return false;
        for (int i = 0; i < DroppedNear.Num; i++)
            DroppedNear[i] = 0;
        auto DroppedSlot = [](long long K) {
            return (unsigned int)(((unsigned long long)K * 0x9E3779B97F4A7C15ull) >> (64 - DroppedNearBits));
        };
        auto Dropped = [&](int e) {
            FV3 Lo, Hi;
            BandBox(e, Lo, Hi);
            if (!EachCell(Lo, Hi, EdgeCell, 1 << 16, [&](long long K) {
                    const unsigned int S = DroppedSlot(K);
                    DroppedNear[(int)(S >> 5)] |= 1u << (S & 31);
                    return true;
                }))
                bDroppedAll = true;
        };
        auto NearDropped = [&](int w) {
            if (bDroppedAll)
                return true;
            const FV3          P = AtPlace(w);
            const unsigned int S = DroppedSlot(Detail::CellKey((long long)floor(P.X / EdgeCell),
                                                               (long long)floor(P.Y / EdgeCell),
                                                               (long long)floor(P.Z / EdgeCell)));
            return (DroppedNear[(int)(S >> 5)] & (1u << (S & 31))) != 0;
        };
        auto MoveOf = [&](int w) {
            if (MoveState[w] == 0 && !Slotted(w))
            {
                bSlotRoom = false;
                return -1;
            }
            if (MoveState[w] == 0 || (Ahead[w] && bAgain && NearDropped(w)))
                MoveInto(w, Nears[0]);
            Ahead[w] = 0;
            return (int)MoveState[w];
        };
        // The moves of the places of List not yet worked out (each once in it), over the rounding's threads: each
        // place's own, from what is read alone.
        TList<int> Ask;
        auto       AskFor = [&](int w) {
            if (MoveState[w] != 0)
                return true;
            MoveState[w] = 3;
            return Slotted(w) && Ask.Add(w);
        };
        auto MovesOf = [&]() {
            RdxSphere::OnThreads(Ask.Num, 256, R.Workers,
                                 [&](int i, int Worker) { MoveInto(Ask[i], Nears[Worker]); });
            for (int i = 0; i < Ask.Num; i++)
                Ahead[Ask[i]] = 1;
        };
        // The edges whose bands place w lies in, left sharp.
        auto DropAt = [&](int w, int& Count) {
            NearOf(AtPlace(w), Nears[0]);
            for (int k = 0; k < Nears[0].Count; k++)
                if (!Drop[Nears[0].Id[k]])
                {
#ifdef LEVELBEVEL_TRACE
                    if (LevelBevelTracePlace() >= 0 && LevelBevelTracePlace() < Places0 + New.Num &&
                        RdxBevel::InBand(Edges[Nears[0].Id[k]], AtPlace(LevelBevelTracePlace()), Radius))
                        printf("  trace %d: edge %d left sharp by place %d at %.3f %.3f %.3f (%s)\n",
                               LevelBevelTracePlace(), Nears[0].Id[k], w, AtPlace(w).X, AtPlace(w).Y, AtPlace(w).Z,
                               (&Count == &M.LeftWorn) ? "no worn solid" : "touching");
#endif
                    Drop[Nears[0].Id[k]] = 1;
                    Dropped(Nears[0].Id[k]);
                    Count++;
                    Fresh++;
                }
            bAgain = true;
        };
        // A place that moves lying on a polygon that is no wall: its edges left sharp (OnOther).
        auto OtherAt = [&](int w) {
            const int Kind = OtherOf[w];
            if (Kind == 0)
                return false;
            int Got = 0;
            DropAt(w, Got);
            M.LeftTouching += Got;
            M.LeftMovers += (Kind == 2) ? Got : 0;
            return true;
        };

        // The faces that take part: those of the pairs and those made again before for a place of theirs that moved;
        // and every face on a side a point goes onto that moves, or on a place that moves of one taking part - where
        // one of those is no face the bevel may cut, the edges of the point or the place are left sharp, and so where
        // the side may be a moved wall's triangle's that is no face (Prep's Unseen). A face that comes to take part has
        // its own places looked at too (Work, in the order they come), here, before anything is cut: its places that
        // move may bring in more. A point that stays may lie on the side of a face that takes no part: a T-junction on a
        // straight side, as the level has its own. A moved wall's triangle that is no face goes with its places where
        // they move: the rounding's triangles are drawn from them.
        TList<unsigned char> Takes;
        TList<int>           Work;
        if (!Takes.Resize(NumFaces) || !Grow())
            return false;
        for (int f = 0; f < NumFaces; f++)
            Takes[f] = 0;
        auto Take = [&](int q) {
            if (Takes[q])
                return true;
            Takes[q] = 1;
            return Work.Add(q);
        };
        for (int k = 0; k < Pairs.Num; k++)
            if (!Take((int)(Pairs[k] >> LineBits)))
                return false;
        for (int f = 0; f < NumFaces; f++)
            if (MustTake[f] && !Take(f))
                return false;
        // The moves the look below asks for, ahead: the points on the rings' sides, and their sides' ends.
        Ask.Clear();
        for (int k = 0; k < OnSides.Num; k++)
            if (!AskFor(OnSides[k].Place) || !AskFor((int)(OnSides[k].Side >> PlaceBits)) ||
                !AskFor((int)(OnSides[k].Side & ((1LL << PlaceBits) - 1))))
                return false;
        MovesOf();
        TList<int> On;
        for (int k = 0; k < OnSides.Num; k++)
        {
            const int w     = OnSides[k].Place;
            const int State = MoveOf(w);
            if (State < 0)
            {
                DropAt(w, M.LeftWorn);
                continue;
            }
            if (State == 2 && OtherAt(w))
                continue;
            // A point that stays on a side whose ends stay is a T-junction on a straight side: the faces that take no
            // part keep the side whole. Where an end moves, the side bends at the point, and every face on it takes it
            // in - where one cannot, the edges that move the end are left sharp.
            const long long Lo = OnSides[k].Side >> PlaceBits, Hi = OnSides[k].Side & ((1LL << PlaceBits) - 1);
            const int       Ends = ((MoveOf((int)Lo) == 2) ? 1 : 0) | ((MoveOf((int)Hi) == 2) ? 2 : 0);
            if (State != 2 && Ends == 0)
                continue;
            const int Why = (State == 2) ? w : (int)((Ends & 1) ? Lo : Hi);   // the place that moves there
            On.Clear();
            if (!FacesOnSide(Lo, Hi, On) || !FacesOnSide(Hi, Lo, On))
                return false;
#ifdef LEVELBEVEL_TRACE
            if (Lo == LevelBevelTracePlace() || Hi == LevelBevelTracePlace())
            {
                printf("  trace %d: a point on the side %lld-%lld (%s, the ends' moves %d), %d faces on it:", LevelBevelTracePlace(),
                       Lo, Hi, (State == 2) ? "it moves" : "it stays", Ends, On.Num);
                for (int j = 0; j < On.Num; j++)
                    printf(" %d (polygon %d%s)", On[j], Prep.Owner[On[j]], Still[On[j]] ? "" : ", not cut");
                printf("; unseen %d %d\n", Prep.Unseen[(int)Lo], Prep.Unseen[(int)Hi]);
            }
#endif
            if (On.Num < 2 && Prep.Unseen[(int)Lo] && Prep.Unseen[(int)Hi])
            {
                DropAt(Why, M.LeftTouching);
                if (State != 2 && Ends == 3)
                    DropAt((int)Hi, M.LeftTouching);
            }
            for (int j = 0; j < On.Num; j++)
                if (!Still[On[j]])
                {
                    DropAt(Why, M.LeftTouching);
                    if (State != 2 && Ends == 3)
                        DropAt((int)Hi, M.LeftTouching);
                }
                else if (!Take(On[j]))
                    return false;
        }
        // The places of the faces taking part, a batch at a time - those come in so far, their moves ahead -, looked at
        // in order: the next batch the faces that came in meanwhile.
        for (int Done = 0; Done < Work.Num;)
        {
            const int End = Work.Num;
            Ask.Clear();
            for (int a = Done; a < End; a++)
                for (int i = 0; i < CornersOf(Prep, R, Work[a]); i++)
                    if (!AskFor(CornerOf(Prep, R, Work[a], i)))
                        return false;
            MovesOf();
            for (int a = Done; a < End; a++)
            {
                const int f = Work[a];
                const int C = CornersOf(Prep, R, f);
                for (int i = 0; i < C; i++)
                {
                    const int w     = CornerOf(Prep, R, f, i);
                    const int State = MoveOf(w);
                    if (State < 0)
                    {
                        DropAt(w, M.LeftWorn);
                        continue;
                    }
                    if (State != 2 || OtherAt(w))
                        continue;
                    On.Clear();
                    if (!FacesOnPlace(w, On))
                        return false;
                    for (int j = 0; j < On.Num; j++)
                        if (!Still[On[j]])
                            DropAt(w, M.LeftTouching);
                        else if (!Take(On[j]))
                            return false;
                }
            }
            Done = End;
        }
        if (!bSlotRoom)
            return false;
        Lap(4);
        if (RdxSphere::Abandoned())
            return false;
        if (bAgain && Fresh == 0)
            break;   // made again, it would come out the same
        if (bAgain)
            continue;
        M.CutRounds++;

        // The points on each side in order along it: sorted by side, then by the share.
        TList<long long> OnOrder;
        if (OnSides.Num >= (1 << OnSideBits))
            return false;
        for (int k = 0; k < OnSides.Num; k++)
            if (!OnOrder.Add((OnSides[k].Side << OnSideBits) | k))
                return false;
        SortOnThreads(OnOrder, R.Workers, SortRoom);
        SortRoom.Free();
        for (int a = 0; a < OnOrder.Num;)
        {
            int b = a + 1;
            while (b < OnOrder.Num && (OnOrder[b] >> OnSideBits) == (OnOrder[a] >> OnSideBits))
                b++;
            for (int i = a + 1; i < b; i++)
                for (int j = i; j > a && OnSides[(int)(OnOrder[j] & ((1 << OnSideBits) - 1))].T <
                                             OnSides[(int)(OnOrder[j - 1] & ((1 << OnSideBits) - 1))].T;
                     j--)
                {
                    const long long Tmp = OnOrder[j];
                    OnOrder[j]          = OnOrder[j - 1];
                    OnOrder[j - 1]      = Tmp;
                }
            a = b;
        }

        // Each face that takes part: its ring with every point on its sides, cut along its lines, cut into triangles
        // (NewTri, from FaceTri0, FaceTriN of them); Changed where a point went onto it or a line cut it. And its pieces'
        // counts of corners (NewPieces, from FacePiece0, FacePieceN of them), each piece's triangles the count less two
        // after the one before's - a piece is cut again where its places moved, should the moves turn one over.
        TList<int>           FaceTri0, FaceTriN, NewTri, FacePiece0, FacePieceN, NewPieces;
        TList<unsigned char> Changed;
        if (!FaceTri0.Resize(NumFaces) || !FaceTriN.Resize(NumFaces) || !Changed.Resize(NumFaces) ||
            !FacePiece0.Resize(NumFaces) || !FacePieceN.Resize(NumFaces))
            return false;
        // The faces are cut a run at a time over the rounding's threads, each into its thread's room (FCutRoom): the
        // places its cuts make numbered from Local0 there, and what each CrossAt it asked gave kept (FCross). The run's
        // faces are then taken in, in their order (TakeCut) - their places numbered on from the level's, their
        // triangles and the edges they leave sharp put in - as if each had been cut after the one before. A face's cut
        // could see no place an earlier face of its run made: where one of those lies within OnLine of a point it put
        // in, or one of those faces made a point on a stretch it put one on (EarlierNear), it is cut again in its turn,
        // every earlier face in by then - what comes out is the same, to the bit, as one face after another.
        // A face's own new place: Local0 + its number among them - past every place a side's key holds (PlaceBits).
        static const int Local0 = 1 << 30;
        // Faces a run: its rooms hold all of its faces' pieces at once - a few megabytes at this length, whatever the
        // level's size - and the threads are started once a run.
        static const int CutRun = 16384;
        // Units: the cells the run's own places are found by (RunNear) - many times OnLine, so a point's OnLine round
        // mostly lies in one; any size finds the same places.
        static const double RunCell = 0.5;
        struct FCross
        {
            int         Lo, Hi;   // the stretch, its lower place first
            FV3         X;        // the point on it
            int         W;        // the place given
            signed char How;      // found on the stretch: 0 a place the face put there, 1 the level's; found within
                                  // OnLine: 2 a place of the face's, 3 the level's; 4 a new place of the face's
        };
        struct FCutFace
        {
            int  Tri0, TriN, New0, NewN, Cross0, CrossN, Drop0, DropN;   // in its room's lists
            int  Piece0, PieceN;
            int  Flat, Missed;                                         // M.PiecesFlat, M.SidesMissed's share
            int  Room;
            bool bTwice;   // its ring came to a place twice: no triangles, the edges in Drops left sharp
        };
        struct FCutRoom
        {
            TList<int>       Ring, RingLine, Pieces, Next, Under, Over, Ids, Signs, Goes, Cut;
            TList<long long> RingSides;
            TList<int>       Tri, Drops, PieceCorners;
            TList<FV3>       New, NewOrig;
            TList<long long> NewKey;   // each new place's small cell (NewCell)
            TList<FCross>    Cross;
            void             Clear()
            {
                Tri.Clear();
                Drops.Clear();
                PieceCorners.Clear();
                New.Clear();
                NewOrig.Clear();
                NewKey.Clear();
                Cross.Clear();
            }
        };
        // Face p cut (Takes), into Room: what F counts of it there.
        auto CutFaceIn = [&](int p, FCutRoom& Room, FCutFace& F) {
            TList<int>&       Ring      = Room.Ring;
            TList<int>&       RingLine  = Room.RingLine;
            TList<int>&       Pieces    = Room.Pieces;
            TList<int>&       Next      = Room.Next;
            TList<int>&       Under     = Room.Under;
            TList<int>&       Over      = Room.Over;
            TList<int>&       Ids       = Room.Ids;
            TList<int>&       Signs     = Room.Signs;
            TList<int>&       Goes      = Room.Goes;
            TList<int>&       Cut       = Room.Cut;
            TList<long long>& RingSides = Room.RingSides;
            auto AtCut   = [&](int w) { return (w >= Local0) ? Room.New[F.New0 + (w - Local0)] : AtPlace(w); };
            auto OrigCut = [&](int w) { return (w >= Local0) ? Room.NewOrig[F.New0 + (w - Local0)] : OrigPlace(w); };
            // CrossAt (bRingSide false) as it would go with the level's places and stretches as they are and the
            // face's own put in: on the stretch, the face's own place before the level's; within OnLine, in each small
            // cell the face's own before the level's - the newer first, as the tables give them.
            auto Cross = [&](int I, int J, int Id) {
                const int    Lo = (I < J) ? I : J, Hi = (I < J) ? J : I;
                const FV3    A = AtCut(Lo), B = AtCut(Hi);
                const double Dl = LineSide(Id, A), Dh = LineSide(Id, B);
                const double T  = Dl / (Dl - Dh);
                FCross       C;
                C.Lo  = Lo;
                C.Hi  = Hi;
                C.X   = A + (B - A) * T;
                C.W   = -1;
                C.How = 0;
                for (int k = Room.Cross.Num - 1; k >= F.Cross0 && C.W < 0; k--)
                {
                    const FCross& O = Room.Cross[k];
                    if (O.How >= 2 && O.Lo == Lo && O.Hi == Hi && RdxSphere::Length(AtCut(O.W) - C.X) <= OnLine)
                        C.W = O.W;
                }
                if (C.W < 0 && Hi < Local0)
                {
                    const long long Key = ((long long)Lo << PlaceBits) | Hi;
                    for (int g = Made.Head[Made.Bucket(Key)]; g >= 0 && C.W < 0; g = Made.Next[g])
                        if (Made.Key[g] == Key && RdxSphere::Length(AtPlace(Made.Item[g]) - C.X) <= OnLine)
                        {
                            C.W   = Made.Item[g];
                            C.How = 1;
                        }
                }
                if (C.W < 0)
                {
                    bool bOwn = false;   // a place of the face's within OnLine: else none of its is looked at
                    for (int k = F.New0; k < Room.New.Num && !bOwn; k++)
                        bOwn = RdxSphere::Length(Room.New[k] - C.X) <= OnLine;
                    const long long Cx = NewCell(C.X.X), Cy = NewCell(C.X.Y), Cz = NewCell(C.X.Z);
                    for (long long z = Cz - 1; z <= Cz + 1 && C.W < 0; z++)
                        for (long long y = Cy - 1; y <= Cy + 1 && C.W < 0; y++)
                            for (long long x = Cx - 1; x <= Cx + 1 && C.W < 0; x++)
                            {
                                const long long K = Detail::CellKey(x, y, z);
                                for (int k = Room.New.Num - 1; bOwn && k >= F.New0 && C.W < 0; k--)
                                    if (Room.NewKey[k] == K && RdxSphere::Length(Room.New[k] - C.X) <= OnLine)
                                    {
                                        C.W   = Local0 + (k - F.New0);
                                        C.How = 2;
                                    }
                                for (int g = NewMayHold(K) ? NewAt.Head[NewAt.Bucket(K)] : -1; g >= 0 && C.W < 0;
                                     g = NewAt.Next[g])
                                    if (NewAt.Key[g] == K &&
                                        RdxSphere::Length(New[NewAt.Item[g] - Places0] - C.X) <= OnLine)
                                    {
                                        C.W   = NewAt.Item[g];
                                        C.How = 3;
                                    }
                            }
                }
                if (C.W < 0)
                {
                    const FV3 Oa = OrigCut(Lo), Ob = OrigCut(Hi);
                    C.W          = Local0 + (Room.New.Num - F.New0);
                    C.How        = 4;
                    if (!Room.New.Add(C.X) || !Room.NewOrig.Add(Oa + (Ob - Oa) * T) ||
                        !Room.NewKey.Add(Detail::CellKey(NewCell(C.X.X), NewCell(C.X.Y), NewCell(C.X.Z))))
                        return -1;
                }
                return Room.Cross.Add(C) ? C.W : -1;
            };
            const int PairAt  = FirstAbove(Pairs, p, LineBits);
            int       PairEnd = PairAt;
            while (PairEnd < Pairs.Num && (int)(Pairs[PairEnd] >> LineBits) == p)
                PairEnd++;
            const int C = CornersOf(Prep, R, p);
            Ring.Clear();
            RingLine.Clear();   // per ring entry, the line that put the point there - -1 a corner
            for (int i = 0; i < C; i++)
            {
                const long long U = CornerOf(Prep, R, p, i), V = CornerOf(Prep, R, p, (i + 1) % C);
                if (!Ring.Add((int)U) || !RingLine.Add(-1))
                    return false;
                const long long Side = (U < V) ? ((U << PlaceBits) | V) : ((V << PlaceBits) | U);
                const int       a    = FirstAbove(OnOrder, Side, OnSideBits);
                int             b    = a;
                while (b < OnOrder.Num && (OnOrder[b] >> OnSideBits) == Side)
                    b++;
                for (int k = 0; k < b - a; k++)
                {
                    const int j = (U < V) ? a + k : b - 1 - k;
                    const FOnSide& Point = OnSides[(int)(OnOrder[j] & ((1 << OnSideBits) - 1))];
                    if (!Ring.Add(Point.Place) || !RingLine.Add(Point.Line))
                        return false;
                    Changed[p] = 1;
                }
            }
            // A ring that comes to one place twice: a face so sharp that a line crosses two of its sides within OnLine
            // of each other, the point it puts there the same for both. No cut goes through it whole: the edges whose
            // lines cut it are left sharp, and the bevel made again without them.
            // The same place twice in a row (two lines crossing a side at one point) is that place once.
            int Once = 0;
            for (int i = 0; i < Ring.Num; i++)
                if (Once == 0 || Ring[Once - 1] != Ring[i])
                {
                    RingLine[Once] = RingLine[i];
                    Ring[Once++]   = Ring[i];
                }
            while (Once > 1 && Ring[Once - 1] == Ring[0])
                Once--;
            Ring.Num     = Once;
            RingLine.Num = Once;
            bool bTwice  = false;
            for (int i = 0; i < Ring.Num; i++)
                for (int j = i + 1; j < Ring.Num; j++)
                    if (Ring[i] == Ring[j])
                    {
                        // The edges whose lines put the point there, on both of its sides.
                        bTwice = true;
                        const int Lines[2] = { RingLine[i], RingLine[j] };
                        for (int k = 0; k < 2; k++)
                            if (Lines[k] >= 0 && !Room.Drops.Add(LineEdge(Lines[k])))
                                return false;
                    }
            if (bTwice)
            {
                F.bTwice = true;
                return true;
            }
            // The ring's sides, for a line found going across one (none should be).
            RingSides.Clear();
            for (int i = 0; i < Ring.Num; i++)
            {
                const long long U = Ring[i], V = Ring[(i + 1) % Ring.Num];
                if (!RingSides.Add((U < V) ? ((U << PlaceBits) | V) : ((V << PlaceBits) | U)))
                    return false;
            }
            Detail::SortSides(RingSides.Data, RingSides.Num);
            // The ring's way round (N, Turn +1): the pieces keep it - where its inside lies, and the triangles' way
            // round. The ring's own turn tells it: a moved wall's thin triangle stands off the plane its normal gives.
            FV3 Sum = RdxSphere::V3(0.0, 0.0, 0.0);
            for (int i = 1; i + 1 < Ring.Num; i++)
            {
                const FV3 O = AtCut(Ring[0]);
                Sum         = Sum + RdxSphere::Cross(AtCut(Ring[i]) - O, AtCut(Ring[i + 1]) - O);
            }
            const double SumLength = RdxSphere::Length(Sum);
            const FV3    N         = (SumLength > 0.0) ? Sum * (1.0 / SumLength) : Prep.Normal[p];
            const double Turn      = 1.0;
            // Cut along each line, as EdgeBevel.h's CutBands cuts: the pieces as a count and then the corners.
            Pieces.Clear();
            if (!Pieces.Add(Ring.Num))
                return false;
            for (int i = 0; i < Ring.Num; i++)
                if (!Pieces.Add(Ring[i]))
                    return false;
            Cut.Clear();
            for (int k = PairAt; k < PairEnd; k++)
            {
                const int Id   = (int)(Pairs[k] & ((1 << LineBits) - 1));
                bool      bOne = false;   // within LinesOne of a line cut along before, all over the polygon
                for (int c = 0; c < Cut.Num && !bOne; c++)
                {
                    if (LineEdge(Cut[c]) == LineEdge(Id))
                        continue;   // an edge's own lines are its band's slices
                    const double Way  = (RdxSphere::Dot(LineInto(Id), LineInto(Cut[c])) >= 0.0) ? 1.0 : -1.0;
                    double       Most = 0.0;
                    for (int i = 0; i < Ring.Num && Most <= LinesOne; i++)
                    {
                        const FV3 P = AtCut(Ring[i]);
                        Most        = MaxOf(Most, fabs(LineSide(Id, P) - Way * LineSide(Cut[c], P)));
                    }
                    bOne = Most <= LinesOne;
                }
                if (bOne)
                    continue;
                if (!Cut.Add(Id))
                    return false;
                auto      Side = [&](int w) {
                    const double D = LineSide(Id, AtCut(w));
                    return (D > OnLine) ? 1 : ((D < -OnLine) ? -1 : 0);
                };
                Next.Clear();
                for (int q = 0; q < Pieces.Num;)
                {
                    const int n = Pieces[q];
                    bool      bUnder = false, bOver = false;
                    for (int c = 0; c < n; c++)
                    {
                        const int Sd = Side(Pieces[q + 1 + c]);
                        bUnder       = bUnder || Sd < 0;
                        bOver        = bOver || Sd > 0;
                    }
                    if (!(bUnder && bOver))
                    {
                        for (int c = 0; c <= n; c++)
                            if (!Next.Add(Pieces[q + c]))
                                return false;
                        q += n + 1;
                        continue;
                    }
                    Changed[p] = 1;
                    // Each corner's side, and the pieces it goes into (Goes: 1 under, 2 over, 3 both). A corner on
                    // the line goes with the side whose run it stands in. Where corners on it stand between the two
                    // sides' runs, the piece's ring runs along the line from the first of them to the last: the
                    // piece's inside along there on the first side's half, they all go with that side and the last
                    // is where the line goes across - else with the other side, from the first; the one where it
                    // goes across in both pieces. The piece beyond such a run would else run back over its corners
                    // to its own corner on the line - their side of the line is no help: they lie on it, often to
                    // the last bit.
                    Signs.Clear();
                    Goes.Clear();
                    int First = -1;
                    for (int c = 0; c < n; c++)
                    {
                        const int Sd = Side(Pieces[q + 1 + c]);
                        if (!Signs.Add(Sd) || !Goes.Add((Sd < 0) ? 1 : ((Sd > 0) ? 2 : 0)))
                            return false;
                        if (First < 0 && Sd != 0)
                            First = c;
                    }
                    for (int s = 1; s < n;)
                    {
                        if (Signs[(First + s) % n] != 0)
                        {
                            s++;
                            continue;
                        }
                        int e = s;
                        while (Signs[(First + e) % n] == 0)
                            e++;
                        const int Before = Signs[(First + s - 1) % n], After = Signs[(First + e) % n];
                        double    Inside = 0.0;   // along the line's way in: the run's inside over it, more than not
                        for (int g = s; g + 1 < e; g++)
                        {
                            const FV3 A = AtCut(Pieces[q + 1 + (First + g) % n]);
                            const FV3 B = AtCut(Pieces[q + 1 + (First + g + 1) % n]);
                            Inside += RdxSphere::Dot(RdxSphere::Cross(N, B - A), LineInto(Id)) * Turn;
                        }
                        const int Across = (Before != After && e - s >= 2 && (Inside > 0.0) == (Before > 0)) ? e - 1 : s;
                        for (int g = s; g < e; g++)
                        {
                            const int Way = (Before == After || g < Across) ? Before : After;
                            Goes[(First + g) % n] = (Before != After && g == Across) ? 3 : ((Way < 0) ? 1 : 2);
                        }
                        s = e;
                    }
                    Under.Clear();
                    Over.Clear();
                    for (int c = 0; c < n; c++)
                    {
                        const int i = Pieces[q + 1 + c], j = Pieces[q + 1 + (c + 1) % n];
                        const int Si = Signs[c], Sj = Signs[(c + 1) % n];
                        if (((Goes[c] & 1) && !Under.Add(i)) || ((Goes[c] & 2) && !Over.Add(i)))
                            return false;
                        if (Si * Sj >= 0)
                            continue;
                        const long long Lo = (i < j) ? i : j, Hi = (i < j) ? j : i;
                        const long long K  = (Lo << PlaceBits) | Hi;
                        const int       At = FirstAbove(RingSides, K, 0);
                        if (Hi < Local0 && At < RingSides.Num && RingSides[At] == K)
                            F.Missed++;
                        const int w = Cross(i, j, Id);
                        if (w < 0 || !Under.Add(w) || !Over.Add(w))
                            return false;
                    }
                    const TList<int>* const Parts[2] = { &Under, &Over };
                    for (int h = 0; h < 2; h++)
                    {
                        if (Parts[h]->Num < 3)
                            continue;
                        if (!Next.Add(Parts[h]->Num))
                            return false;
                        for (int c = 0; c < Parts[h]->Num; c++)
                            if (!Next.Add((*Parts[h])[c]))
                                return false;
                    }
                    q += n + 1;
                }
                if (!Pieces.Resize(Next.Num))
                    return false;
                for (int c = 0; c < Next.Num; c++)
                    Pieces[c] = Next[c];
            }
            // The pieces into triangles.
            for (int q = 0; q < Pieces.Num;)
            {
                const int n = Pieces[q];
                Ids.Clear();
                for (int c = 0; c < n; c++)
                    if (!Ids.Add(Pieces[q + 1 + c]))
                        return false;
                q += n + 1;
                // A corner that came out the same place twice in a row (a cut at a place already there) once.
                int Kept = 0;
                for (int c = 0; c < Ids.Num; c++)
                    if (Kept == 0 || Ids[Kept - 1] != Ids[c])
                        Ids[Kept++] = Ids[c];
                while (Kept > 1 && Ids[Kept - 1] == Ids[0])
                    Kept--;
                Ids.Num = Kept;
                if (Ids.Num < 3)
                    continue;
                if (!Room.PieceCorners.Add(Ids.Num))
                    return false;
                if (!CutIntoTriangles(AtCut, Ids, N, Turn, Room.Tri))
                {
                    // What is left has its corners on one line: a fan of triangles of no area over it, which no ray
                    // hits and no pixel shows, keeps every side's twin - the level closed.
                    F.Flat++;
                    Room.Tri.Num -= Room.Tri.Num % 3;
                    for (int c = 1; c + 1 < Ids.Num; c++)
                        if (!Room.Tri.Add(Ids[0]) || !Room.Tri.Add(Ids[c]) || !Room.Tri.Add(Ids[c + 1]))
                            return false;
                }
            }
            return true;
        };
        auto CutFace = [&](int p, FCutRoom& Room, FCutFace& F) {
            F.Tri0   = Room.Tri.Num;
            F.New0   = Room.New.Num;
            F.Cross0 = Room.Cross.Num;
            F.Drop0  = Room.Drops.Num;
            F.Piece0 = Room.PieceCorners.Num;
            F.Flat   = 0;
            F.Missed = 0;
            F.bTwice = false;
            const bool bOk = CutFaceIn(p, Room, F);
            F.TriN   = (Room.Tri.Num - F.Tri0) / 3;
            F.NewN   = Room.New.Num - F.New0;
            F.CrossN = Room.Cross.Num - F.Cross0;
            F.DropN  = Room.Drops.Num - F.Drop0;
            F.PieceN = Room.PieceCorners.Num - F.Piece0;
#ifdef LEVELBEVEL_TRACE
            for (int i = 0; i < CornersOf(Prep, R, p); i++)
                if (CornerOf(Prep, R, p, i) == LevelBevelTracePlace())
                {
                    printf("  trace %d: face %d (polygon %d, %s) cut into %d triangles, %d new places, %d crossings\n",
                           LevelBevelTracePlace(), p, Prep.Owner[p], (p < Prep.RingFaces) ? "ring" : "moved triangle",
                           F.TriN, F.NewN, F.CrossN);
                    // Its corners - a ring face's with the places put in on its sides - and its polygon's own count.
                    for (int c = 0; c < CornersOf(Prep, R, p); c++)
                    {
                        const int w = CornerOf(Prep, R, p, c);
                        printf("    corner %d: place %d at %.3f %.3f %.3f (of the polygon's %d)\n", c, w, R.Moved[w].X,
                               R.Moved[w].Y, R.Moved[w].Z, (p < Prep.RingFaces) ? R.RingCount[Prep.RingPoly[p]] : 3);
                    }
                }
#endif
            return bOk;
        };
        {
            TList<FCutFace> RunFaces;
            FCutRoom        Rooms[RdxSphere::WorkersMost + 1];   // the threads', and one for a face cut again
            FCutRoom&       Again = Rooms[RdxSphere::WorkersMost];
            // What the run's faces taken in so far put in: the stretches between two of the level's places as the run
            // began (RunPlaces) with their places, and the places they made, by cells RunCell wide.
            Detail::FCells  RunMade, RunNear;
            int             RunPlaces = 0;
            auto            RunCellOf = [](double V) { return (long long)floor(V / RunCell); };
            auto            EarlierNear = [&](const FCutRoom& Room, const FCutFace& F) {
                for (int k = F.Cross0; k < F.Cross0 + F.CrossN; k++)
                {
                    const FCross& C = Room.Cross[k];
                    if (C.How == 0)
                        continue;
                    if (C.Hi < Local0)
                    {
                        const long long Key = ((long long)C.Lo << PlaceBits) | C.Hi;
                        for (int g = RunMade.Head[RunMade.Bucket(Key)]; g >= 0; g = RunMade.Next[g])
                            if (RunMade.Key[g] == Key && RdxSphere::Length(AtPlace(RunMade.Item[g]) - C.X) <= OnLine)
                                return true;
                    }
                    if (C.How < 2)
                        continue;
                    for (long long z = RunCellOf(C.X.Z - OnLine); z <= RunCellOf(C.X.Z + OnLine); z++)
                        for (long long y = RunCellOf(C.X.Y - OnLine); y <= RunCellOf(C.X.Y + OnLine); y++)
                            for (long long x = RunCellOf(C.X.X - OnLine); x <= RunCellOf(C.X.X + OnLine); x++)
                            {
                                const long long K = Detail::CellKey(x, y, z);
                                for (int g = RunNear.Head[RunNear.Bucket(K)]; g >= 0; g = RunNear.Next[g])
                                    if (RunNear.Key[g] == K && RdxSphere::Length(AtPlace(RunNear.Item[g]) - C.X) <= OnLine)
                                        return true;
                            }
                }
                return false;
            };
            // Face p's cut taken in: its places numbered on from the level's, as CrossAt would have numbered them.
            auto TakeCut = [&](int p, const FCutRoom& Room, const FCutFace& F, bool bRun) {
                const int Base  = Places0 + New.Num;
                auto      Final = [&](int w) { return (w >= Local0) ? Base + (w - Local0) : w; };
                for (int k = 0; k < F.NewN; k++)
                {
                    // A place past what a side's key holds (PlaceBits): the level too large for the bevel, which
                    // leaves it as the rounding made it.
                    const int w = Base + k;
                    if (w >= (1 << PlaceBits))
                        return false;
                    const FV3& X = Room.New[F.New0 + k];
                    if (!New.Add(X) || !NewOrig.Add(Room.NewOrig[F.New0 + k]) || !AddNewAt(Room.NewKey[F.New0 + k], w))
                        return false;
                    if (bRun && !RunNear.Add(Detail::CellKey(RunCellOf(X.X), RunCellOf(X.Y), RunCellOf(X.Z)), w))
                        return false;
                }
                for (int k = F.Cross0; k < F.Cross0 + F.CrossN; k++)
                {
                    const FCross& C = Room.Cross[k];
                    if (C.How < 2)
                        continue;
                    const long long Key = ((long long)Final(C.Lo) << PlaceBits) | Final(C.Hi);
                    if (!Made.Add(Key, Final(C.W)))
                        return false;
                    if (bRun && Final(C.Hi) < RunPlaces && !RunMade.Add(Key, Final(C.W)))
                        return false;
                }
                FaceTri0[p] = NewTri.Num / 3;
                FaceTriN[p] = F.TriN;
                for (int k = F.Tri0; k < F.Tri0 + F.TriN * 3; k++)
                    if (!NewTri.Add(Final(Room.Tri[k])))
                        return false;
                FacePiece0[p] = NewPieces.Num;
                FacePieceN[p] = F.PieceN;
                for (int k = F.Piece0; k < F.Piece0 + F.PieceN; k++)
                    if (!NewPieces.Add(Room.PieceCorners[k]))
                        return false;
                for (int k = F.Drop0; k < F.Drop0 + F.DropN; k++)
                    if (!Drop[Room.Drops[k]])
                    {
                        Drop[Room.Drops[k]] = 1;
                        Dropped(Room.Drops[k]);
                        M.LeftTouching++;
                        Fresh++;
                    }
                bAgain = bAgain || F.bTwice;
                M.PiecesFlat += F.Flat;
                M.SidesMissed += F.Missed;
                return true;
            };
            const int Run = (Threads > 1) ? CutRun : 1;
            if (!RunFaces.Resize(Run))
                return false;
            for (int r0 = 0; r0 < NumFaces; r0 += Run)
            {
                const int r1 = (NumFaces - r0 > Run) ? r0 + Run : NumFaces;
                for (int w = 0; w < Threads; w++)
                    Rooms[w].Clear();
                std::atomic<bool> NoRoomCut(false);
                RdxSphere::OnThreads(r1 - r0, 64, Threads, [&](int i, int Worker) {
                    const int p = r0 + i;
                    FCutFace& F = RunFaces[i];
                    F.Room      = Worker;
                    Changed[p]  = 0;
                    if (Takes[p] && !NoRoomCut && !CutFace(p, Rooms[Worker], F))
                        NoRoomCut = true;
                });
                if (NoRoomCut)
                    return false;
                const bool bRun = r1 - r0 > 1;
                if (bRun)
                {
                    int Entries = 0, Places = 0;
                    for (int w = 0; w < Threads; w++)
                    {
                        Entries += Rooms[w].Cross.Num;
                        Places += Rooms[w].New.Num;
                    }
                    if (!RunMade.Init(Entries) || !RunNear.Init(Places))
                        return false;
                    RunPlaces = Places0 + New.Num;
                }
                for (int i = 0; i < r1 - r0; i++)
                {
                    const int p = r0 + i;
                    if (!Takes[p])
                    {
                        FaceTri0[p]   = NewTri.Num / 3;
                        FaceTriN[p]   = 0;
                        FacePiece0[p] = NewPieces.Num;
                        FacePieceN[p] = 0;
                        continue;
                    }
                    const FCutFace& F = RunFaces[i];
                    if (bRun && EarlierNear(Rooms[F.Room], F))
                    {
                        FCutFace G;
                        Again.Clear();
                        Changed[p] = 0;
                        if (!CutFace(p, Again, G) || !TakeCut(p, Again, G, bRun))
                            return false;
                        M.CutAgain++;
                    }
                    else if (!TakeCut(p, Rooms[F.Room], F, bRun))
                        return false;
                }
            }
        }

        Lap(5);
        if (RdxSphere::Abandoned())
            return false;
        // The places of the new triangles the earlier look did not reach - the points the cuts made inside the faces,
        // and the places of the faces that take part for another's sake. One with no worn solid under it leaves its
        // bands' edges sharp; one of the rounding's that moves, a face on it the bevel may cut that took no part takes
        // part the next time - and where one on it is no such face, its bands' edges are left sharp.
        if (!Grow())
            return false;
        Ask.Clear();
        for (int t = 0; t < NewTri.Num; t++)
            if (!AskFor(NewTri[t]))
                return false;
        MovesOf();
        for (int a = 0; a < Ask.Num; a++)
        {
            const int w     = Ask[a];
            const int State = MoveOf(w);
            if (State < 0)
            {
                DropAt(w, M.LeftWorn);
                continue;
            }
            if (State != 2 || OtherAt(w))
                continue;
            if (w >= Places0)
                continue;
            On.Clear();
            if (!FacesOnPlace(w, On))
                return false;
            for (int j = 0; j < On.Num; j++)
            {
                if (Takes[On[j]])
                    continue;
                if (Still[On[j]])
                {
                    MustTake[On[j]] = 1;
                    bAgain          = true;
                    Fresh++;
                }
                else
                    DropAt(w, M.LeftTouching);
            }
        }
        if (!bSlotRoom)
            return false;
        if (bAgain && Fresh == 0)
            break;
        if (bAgain)
        {
            Lap(6);
            continue;
        }

        // Where the moves turn a triangle of a piece over - against the way it faced where the cut put its corners, more
        // than a sliver either way, as the checks take it - the piece is cut again where its places moved
        // (CutMovedIntoTriangles), into as many triangles, in their place: a ball's corner pushing into a wedge of a face
        // turns the wedge's thin triangles over, where the moved piece still has its ears. Where it has none, or a face's
        // triangles are not its pieces' (none should be), the cut stays.
        {
            auto             NowAt = [&](int w) { return (MoveState[w] == 2) ? MoveAt[MoveSlot[w]] : AtPlace(w); };
            std::atomic<int> Turned(0), CutMoved(0);
            RdxSphere::OnThreads(NumFaces, 256, Threads, [&](int f, int) {
                int Tris = 0;
                for (int k = FacePiece0[f]; k < FacePiece0[f] + FacePieceN[f]; k++)
                    Tris += NewPieces[k] - 2;
                if (Tris != FaceTriN[f])
                    return;
                int t = FaceTri0[f];
                for (int k = FacePiece0[f]; k < FacePiece0[f] + FacePieceN[f]; k++)
                {
                    const int Corners = NewPieces[k];
                    const int Own     = Corners - 2;
                    bool             bTurned = false;
                    for (int i = t; i < t + Own && !bTurned; i++)
                    {
                        const int A = NewTri[i * 3], B = NewTri[i * 3 + 1], C = NewTri[i * 3 + 2];
                        if (MoveState[A] != 2 && MoveState[B] != 2 && MoveState[C] != 2)
                            continue;
                        const FV3 Then = RdxSphere::Cross(AtPlace(B) - AtPlace(A), AtPlace(C) - AtPlace(A));
                        const FV3 Now  = RdxSphere::Cross(NowAt(B) - NowAt(A), NowAt(C) - NowAt(A));
                        bTurned        = RdxSphere::Dot(Now, Then) < 0.0 &&
                                  (0.5 * RdxSphere::Length(Then) >= TurnSliver || 0.5 * RdxSphere::Length(Now) >= TurnSliver);
                    }
                    if (bTurned)
                        Turned++;
                    // The piece's ring back from its triangles, its way round where the cut put it, and its places now.
                    int Ids[MostMoved];
                    if (bTurned && Corners <= MostMoved && RingOfTriangles(&NewTri[t * 3], Own, Ids, Corners))
                    {
                        FV3 Way = RdxSphere::V3(0.0, 0.0, 0.0), P[MostMoved];
                        int Out[3 * MostMoved];
                        for (int c = 1; c + 1 < Corners; c++)
                            Way = Way + RdxSphere::Cross(AtPlace(Ids[c]) - AtPlace(Ids[0]),
                                                         AtPlace(Ids[c + 1]) - AtPlace(Ids[0]));
                        const double L = RdxSphere::Length(Way);
                        for (int c = 0; c < Corners; c++)
                            P[c] = NowAt(Ids[c]);
                        if (L > 0.0 && CutMovedIntoTriangles(P, Ids, Corners, Way * (1.0 / L), Out))
                        {
                            for (int i = 0; i < 3 * Own; i++)
                                NewTri[t * 3 + i] = Out[i];
                            CutMoved++;
                        }
                    }
                    t += Own;
                }
            });
            M.PiecesTurned += Turned;
            M.PiecesCutMoved += CutMoved;
            // The pieces were for this alone: let go before R grows.
            NewPieces.Free();
            FacePiece0.Free();
            FacePieceN.Free();
        }

        // A still polygon's ring no point went onto, no line cut and none of whose places moved stays the engine's. A
        // moved wall's triangle no point went onto and no line cut stays as it was: its places go where they move.
        for (int f = 0; f < Prep.RingFaces; f++)
            for (int t = FaceTri0[f] * 3; t < (FaceTri0[f] + FaceTriN[f]) * 3 && !Changed[f]; t++)
                if (MoveState[NewTri[t]] == 2)
                    Changed[f] = 1;
        auto Pieced = [&](int f) { return Changed[f] && FaceTriN[f] > 0; };

        // Into R: the rounding's places that moved, the new places, and each changed polygon's triangles - a still
        // polygon's its ring's pieces; a moved one's its own, each changed one's pieces in its place, in a new run - the
        // room for all of it had first, R left as the rounding made it where there is none.
        int TrisIn = 0;
        for (int f = 0; f < Prep.RingFaces; f++)
            if (Pieced(f))
                TrisIn += FaceTriN[f];
        for (int a = Prep.RingFaces; a < NumFaces;)
        {
            const int p = Prep.Owner[a];
            int       b = a, More = 0;
            bool      bAny = false;
            for (; b < NumFaces && Prep.Owner[b] == p; b++)
                if (Pieced(b))
                {
                    bAny = true;
                    More += FaceTriN[b] - 1;
                }
            TrisIn += bAny ? R.PolyTris[p] + More : 0;
            a = b;
        }
        // What only the making needed, let go before R grows: the bevel's lists are at their most here.
        Pairs.Free();
        OnSides.Free();
        OnOrder.Free();
        Made.Free();
        NewAt.Free();
        EdgeGrid.Free();
        FaceGrid.Free();
        PlaceKeys.Free();
        Prep.SideKeys.Free();
        for (int w = 0; w < Threads; w++)
        {
            Seens[w].Free();
            PairsOf[w].Free();
        }
        // The room exactly: kept for the level's whole stay, a doubling's spare half would be held as long.
        if (!R.Orig.ReserveExact(R.Orig.Num + New.Num) || !R.Moved.ReserveExact(R.Moved.Num + New.Num) ||
            !R.PolyFirst.Reserve(NumPolys) || !R.PolyTris.Reserve(NumPolys) ||
            !R.Tri.ReserveExact(R.Tri.Num + TrisIn * 3) || !R.TriTurn.ReserveExact(R.TriTurn.Num + TrisIn) ||
            (R.PlaceHeld.Num > 0 && !R.PlaceHeld.ReserveExact(R.Orig.Num + New.Num)))
            return false;
        M.Points = New.Num;
#ifdef LEVELBEVEL_TRACE
        LevelBevelTraceBefore().Clear();
        LevelBevelTracePlaces0() = Places0;
        for (int w = 0; w < Places0 + New.Num; w++)
            LevelBevelTraceBefore().Add(AtPlace(w));
#endif
        for (int w = 0; w < Places0; w++)
            if (MoveState[w] == 2)
                R.Moved[w] = MoveAt[MoveSlot[w]];
        for (int k = 0; k < New.Num; k++)
            if (!R.Orig.Add(NewOrig[k]) ||
                !R.Moved.Add((MoveState[Places0 + k] == 2) ? MoveAt[MoveSlot[Places0 + k]] : New[k]))
                return false;
        if (!R.BevelNormal.ReserveExact(R.Orig.Num))
            return false;
        R.BevelNormal.Num = R.Orig.Num;
        for (int w = 0; w < R.Orig.Num; w++)
            R.BevelNormal[w] = (w < MoveState.Num && MoveState[w] == 2) ? MoveNrm[MoveSlot[w]] : 0u;
        // The places held back before a still polygon (LevelRound.h's HoldBehind) one a place: the new ones are not.
        while (R.PlaceHeld.Num > 0 && R.PlaceHeld.Num < R.Orig.Num)
            if (!R.PlaceHeld.Add(0))
                return false;
        if (R.PolyFirst.Num < NumPolys || R.PolyTris.Num < NumPolys)
        {
            const int Had = R.PolyTris.Num;
            if (!R.PolyFirst.Resize(NumPolys) || !R.PolyTris.Resize(NumPolys))
                return false;
            for (int p = Had; p < NumPolys; p++)
                R.PolyFirst[p] = R.PolyTris[p] = 0;
        }
        for (int f = 0; f < Prep.RingFaces; f++)
        {
            if (!Pieced(f))
                continue;
            const int p = Prep.RingPoly[f];
            M.Polygons++;
            M.TrisBefore += R.RingCount[p] - 2;
            M.TrisAfter += FaceTriN[f];
            R.PolyFirst[p] = R.Tri.Num / 3;
            R.PolyTris[p]  = FaceTriN[f];
            for (int t = FaceTri0[f] * 3; t < (FaceTri0[f] + FaceTriN[f]) * 3; t++)
                if (!R.Tri.Add(NewTri[t]))
                    return false;
            for (int t = 0; t < FaceTriN[f]; t++)
                if (!R.TriTurn.Add(0.0f))
                    return false;
            R.Touched++;
        }
        for (int a = Prep.RingFaces; a < NumFaces;)
        {
            const int p = Prep.Owner[a];
            int       b = a;
            bool      bAny = false;
            for (; b < NumFaces && Prep.Owner[b] == p; b++)
                bAny = bAny || Pieced(b);
            if (bAny)
            {
                const int First = R.Tri.Num / 3, Was = R.PolyTris[p];
                int       k     = a;   // the polygon's next face, by its triangle
                for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + Was; t++)
                {
                    while (k < b && Prep.TriFace[k - Prep.RingFaces] < t)
                        k++;
                    const float Turn = R.TriTurn[t];
                    if (k < b && Prep.TriFace[k - Prep.RingFaces] == t && Pieced(k))
                    {
                        for (int i = FaceTri0[k] * 3; i < (FaceTri0[k] + FaceTriN[k]) * 3; i++)
                            if (!R.Tri.Add(NewTri[i]))
                                return false;
                        for (int i = 0; i < FaceTriN[k]; i++)
                            if (!R.TriTurn.Add(Turn))
                                return false;
                        continue;
                    }
                    const int A = R.Tri[t * 3], B = R.Tri[t * 3 + 1], C = R.Tri[t * 3 + 2];
                    if (!R.Tri.Add(A) || !R.Tri.Add(B) || !R.Tri.Add(C) || !R.TriTurn.Add(Turn))
                        return false;
                }
                M.Polygons++;
                M.TrisBefore += Was;
                M.TrisAfter += R.Tri.Num / 3 - First;
                R.PolyFirst[p] = First;
                R.PolyTris[p]  = R.Tri.Num / 3 - First;
            }
            a = b;
        }
        // The runs the moved polygons made again left behind taken out: every polygon's run moved down, in the order
        // they lie, over the gaps - no copy of the list - and the room cut to what is left.
        {
            TList<long long> Runs;   // a run's first triangle, and its polygon
            for (int p = 0; p < NumPolys && p < R.PolyTris.Num; p++)
                if (R.PolyTris[p] > 0 && !Runs.Add(((long long)R.PolyFirst[p] << 32) | p))
                    return false;
            Detail::SortSides(Runs.Data, Runs.Num);
            int To = 0;
            for (int i = 0; i < Runs.Num; i++)
            {
                const int p = (int)(Runs[i] & 0xFFFFFFFF), From = R.PolyFirst[p], N = R.PolyTris[p];
                if (From != To)
                {
                    memmove(R.Tri.Data + To * 3, R.Tri.Data + From * 3, sizeof(int) * 3 * (size_t)N);
                    memmove(R.TriTurn.Data + To, R.TriTurn.Data + From, sizeof(float) * (size_t)N);
                    R.PolyFirst[p] = To;
                }
                To += N;
            }
            R.Tri.Num     = To * 3;
            R.TriTurn.Num = To;
            R.Tri.Shrink();
            R.TriTurn.Shrink();
        }
        Lap(6);
        return true;
    }
    // Made again too often: left as the rounding made it.
    M.Rounds = MostRounds;
    return true;
}

} // namespace RdxLevelRound

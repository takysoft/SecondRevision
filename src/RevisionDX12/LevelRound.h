// SecondRevision - the level rounded (SpherifierDistance=<units>, SpherifierAngle=<degrees>): the level's own polygons
// rounded by the very rule the models are (MeshRound.h), in every direction - a pool's wall, a column, a dome, a pipe, a
// round hole in a floor - once a level, the same from wherever it is looked at.
//
// - The level's polygons that are drawn and stay where the level left them (Spherify.h's UseFor: no mover's, no portal,
//   no mirror) are welded where their corners are within WeldDist, and a corner that lies on another polygon's edge is
//   put into that edge (the BSP's T-junctions), so that the two sides of every edge are the same edge.
// - Each polygon is cut into triangles over its own corners, the T-junctions' among them (the shortest cuts, none of
//   them a sliver of no area or one lying along an edge), and the triangles of the whole level make one model of flat
//   parts for MeshRound.h's
//   shape: the polygons that lie in one plane, the fans, the soft edges and the creases, each edge's terms. The open
//   edges - where the drawn level ends against a mover, a portal or the sky - stay where they are.
// - Every triangle is then cut on the CPU, as the models' patches are cut on the card, and every point of it moved as
//   the patches move theirs (MeshRound.h's PatchMoveAt, held to Cap): each edge into as many pieces as its curve needs to
//   stand off them by no more than Sag at their middles - between two flat polygons, one of them a wall the relief may
//   be drawn on (Use_Relief), all along them -, the same pieces from both of its sides; the inside of the triangle into
//   as many as its edges' middles ask for, its points along an edge cut fewer times taken to that edge's own points (the
//   grid's triangles left with no area dropped), and an edge's points the grid does not take joined to it by a fan from
//   the middle of the grid's triangle there, so that no point of one side lies on the other side's piece. A polygon
//   nothing of which moves is left to the engine to draw as it is.
//
// The result is Spherify.h's FResult - each polygon's triangles where they were and where they are now - which the
// renderer draws, shadows and traces in the level's place. Engine-independent: the unit tests and tools\LevelRoundCheck
// run the same code.
#pragma once

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "MeshRound.h"
#include "Spherify.h"

namespace RdxLevelRound
{

using RdxSphere::FLightPart;
using RdxSphere::FLightSide;
using RdxSphere::FPolyIn;
using RdxSphere::FResult;
using RdxSphere::FV3;
using RdxSphere::MaxOf;
using RdxSphere::MinOf;
using RdxSphere::TList;

static const double WeldDist = 0.01;   // units: places this near are one
static const double OnEdge   = 0.05;   // units: a corner this near another polygon's edge lies on it
static const double GridCell = 32.0;   // units: the cells the T-junctions are looked for in
static const float  Sag      = 0.25f;  // units: the most a cut piece of a curve stands off it
static const int    MostCuts = 16;     // pieces an edge is cut into, at most
// A triangle turned over that does not show: all of it within TurnInPlane of its polygon's plane, or a sliver of
// less than TurnSliver square units (tools\LevelRoundCheck counts them so).
static const double TurnInPlane = 0.05, TurnSliver = 0.1;
// The faces laid at most this many times: each time again those on a finer edge taken back, where a face still turns
// over where it shows - an edge's points the grids do not take reach no further than the faces on it (their fans).
static const int    TurnPasses = 4;
static const int    MostRing = 64;     // corners a polygon may have, its T-junctions' with them, to be rounded
static const double PolyCell = 64.0;   // units: the plan grid the moved polygons are found by (PolygonAt)
static const double Straight = 2.0 * OnEdge;   // units: a corner this near the middle of a triangle's longest side lies on it
// What a cut along a sheet's side across a wall takes off the length of the wall's cuts (step 3) - more than all its
// other cuts can come to, so the wall is cut along the sheet's side wherever it can be.
static const double SheetCutGain = 1e9;
static const int    FaceTermFloats = 4 * RdxMeshRound::EDGE_SLOTS * 4;   // a face's edges' terms, kept (FaceTerm)

namespace Detail
{

// A heap of N long longs, the greatest on top, mended down from Root.
inline void SiftSides(long long* A, int Root, int N)
{
    const long long V = A[Root];
    for (int Child = 2 * Root + 1; Child < N; Child = 2 * Root + 1)
    {
        if (Child + 1 < N && A[Child] < A[Child + 1])
            Child++;
        if (!(V < A[Child]))
            break;
        A[Root] = A[Child];
        Root    = Child;
    }
    A[Root] = V;
}

// N long longs sorted, the least first - inline, where the C library's qsort calls a function for every comparison: a
// quicksort on the middle of three, the larger part kept for later, insertion for a part of sixteen or fewer, and a
// heapsort for a part that keeps splitting unevenly. Equal ones being the same number, the order is the one any sort
// gives.
inline void SortSides(long long* A, int N)
{
    struct FPart
    {
        int Lo, Hi, Depth;
    };
    FPart Parts[64];
    int   Top   = 0;
    int   Depth = 0;
    for (int n = N; n > 1; n >>= 1)
        Depth += 2;
    if (N > 1)
        Parts[Top++] = { 0, N - 1, Depth };
    while (Top > 0)
    {
        Top--;
        int Lo = Parts[Top].Lo, Hi = Parts[Top].Hi, D = Parts[Top].Depth;
        while (Hi - Lo >= 16)
        {
            if (D-- <= 0)
            {
                long long* const B     = A + Lo;
                const int        Count = Hi - Lo + 1;
                for (int i = Count / 2 - 1; i >= 0; i--)
                    SiftSides(B, i, Count);
                for (int i = Count - 1; i > 0; i--)
                {
                    const long long T = B[0];
                    B[0]              = B[i];
                    B[i]              = T;
                    SiftSides(B, 0, i);
                }
                Lo = Hi;
                break;
            }
            // The first, the middle and the last in order: the middle one splits, the other two stop the walks.
            const int Mid = Lo + (Hi - Lo) / 2;
            if (A[Mid] < A[Lo])
            {
                const long long T = A[Mid];
                A[Mid]            = A[Lo];
                A[Lo]             = T;
            }
            if (A[Hi] < A[Mid])
            {
                const long long T = A[Hi];
                A[Hi]             = A[Mid];
                A[Mid]            = T;
                if (A[Mid] < A[Lo])
                {
                    const long long U = A[Mid];
                    A[Mid]            = A[Lo];
                    A[Lo]             = U;
                }
            }
            const long long Pivot = A[Mid];
            int             i = Lo, j = Hi;
            while (i <= j)
            {
                while (A[i] < Pivot)
                    i++;
                while (Pivot < A[j])
                    j--;
                if (i <= j)
                {
                    const long long T = A[i];
                    A[i]              = A[j];
                    A[j]              = T;
                    i++;
                    j--;
                }
            }
            if (j - Lo < Hi - i)
            {
                if (i < Hi)
                    Parts[Top++] = { i, Hi, D };
                Hi = j;
            }
            else
            {
                if (Lo < j)
                    Parts[Top++] = { Lo, j, D };
                Lo = i;
            }
        }
        for (int k = Lo + 1; k <= Hi; k++)
        {
            const long long V = A[k];
            int             m = k - 1;
            while (m >= Lo && V < A[m])
            {
                A[m + 1] = A[m];
                m--;
            }
            A[m + 1] = V;
        }
    }
}

inline long long CellKey(long long X, long long Y, long long Z)
{
    return ((X & 0x1FFFFF) << 42) | ((Y & 0x1FFFFF) << 21) | (Z & 0x1FFFFF);
}

// A hash of cells to items, each cell's items chained.
struct FCells
{
    TList<int>       Head;    // a bucket: its first entry
    TList<long long> Key;     // an entry: its cell,
    TList<int>       Item;    // its item,
    TList<int>       Next;    // and the next entry of its bucket
    int              Mask = 0;

    bool Init(int Expected)
    {
        int Size = 64;
        while (Size < Expected * 2)
            Size *= 2;
        if (!Head.Resize(Size))
            return false;
        Mask = Size - 1;
        for (int i = 0; i < Size; i++)
            Head[i] = -1;
        Key.Clear();
        Item.Clear();
        Next.Clear();
        return true;
    }
    int Bucket(long long K) const
    {
        const unsigned long long H = (unsigned long long)K * 0x9E3779B97F4A7C15ull;
        return (int)(H >> 40) & Mask;
    }
    bool Add(long long K, int I)
    {
        const int B = Bucket(K);
        if (!Key.Add(K) || !Item.Add(I) || !Next.Add(Head[B]))
            return false;
        Head[B] = Key.Num - 1;
        return true;
    }
};

inline double Dist2(const FV3& A, const FV3& B)
{
    const double X = A.X - B.X, Y = A.Y - B.Y, Z = A.Z - B.Z;
    return X * X + Y * Y + Z * Z;
}

// Triangle f's normal (Corner: three places a triangle), of length one; none where it has no area.
inline FV3 FaceNormal(const TList<FV3>& Place, const TList<int>& Corner, int f)
{
    const FV3    A = Place[Corner[f * 3]];
    const FV3    N = RdxSphere::Cross(Place[Corner[f * 3 + 1]] - A, Place[Corner[f * 3 + 2]] - A);
    const double L = RdxSphere::Length(N);
    return (L > 1e-12) ? N * (1.0 / L) : RdxSphere::V3(0.0, 0.0, 0.0);
}

// The way into triangle f (normal N) from its edge between the places a and b, in its plane, of length one: towards its
// third corner. None where the edge has no length.
inline FV3 IntoFace(const TList<FV3>& Place, const TList<int>& Corner, int f, int a, int b, const FV3& N)
{
    FV3          W = RdxSphere::Cross(N, Place[b] - Place[a]);
    const double L = RdxSphere::Length(W);
    if (!(L > 1e-12))
        return RdxSphere::V3(0.0, 0.0, 0.0);
    W     = W * (1.0 / L);
    int c = Corner[f * 3];
    for (int k = 0; k < 3; k++)
        if (Corner[f * 3 + k] != a && Corner[f * 3 + k] != b)
            c = Corner[f * 3 + k];
    return (RdxSphere::Dot(Place[c] - Place[a], W) < 0.0) ? W * -1.0 : W;
}

// The angle at corner C of the triangle with the corners A and B.
inline double AngleAt(const FV3& C, const FV3& A, const FV3& B)
{
    const FV3 U = A - C, W = B - C;
    return atan2(RdxSphere::Length(RdxSphere::Cross(U, W)), RdxSphere::Dot(U, W));
}

// Whether the triangle P, Q, R keeps the way round of the normal N with some area: twice its area along N more than a
// millionth of its longest side's square.
inline bool KeepsWay(const FV3& P, const FV3& Q, const FV3& R, const FV3& N)
{
    const FV3    U = Q - P, W = R - P;
    const double Long = MaxOf(RdxSphere::Dot(U, U), MaxOf(RdxSphere::Dot(W, W), RdxSphere::Dot(W - U, W - U)));
    return RdxSphere::Dot(RdxSphere::Cross(U, W), N) > 1e-6 * Long;
}

// The level's flat polygons as one piece each (Round's step 5) - as the brushes made them, before the BSP cut them: the
// shape's triangles with every place taken out that is no corner of the shape, so that nothing the cutting left pins the
// rounded surface. A place goes that lies inside a flat polygon (where its pieces meet), or on a straight run of two
// true edges of one kind (a T-junction's, or a polygon's own corner on the line of two others): collapsed onto a
// neighbour - one on a run onto the next along it - where no triangle round it turns over or is left with no area, pass
// by pass until no more goes; one that cannot go stays a corner (Stuck). Each flat polygon is then cut again by the
// Delaunay rule: an edge between two of its triangles is turned where the corners across it see it at more than half a
// turn together. The pieces are made of the level's places: three a piece (Tri, the way round its triangles went), and
// the flat polygon each lies in (Region: FShape::Poly, its lowest triangle). Faces Skip names (none: null) are left out:
// a sheet that goes along with the walls it lies across (FShape::SheetAlong) has no piece of its own - its corners on the
// walls' runs would be taken out along them and drag it onto the runs' ends, and the edges there would be three faces'.
// Its places move with the walls' surface round them, its middle not at all.
struct FCoarse
{
    TList<int>           Tri;
    TList<int>           Region;
    TList<unsigned char> Taken;   // a place: taken out - it moves with the surface round it
    TList<unsigned char> Still;   // a place taken out of a run of edges that do not bow: it stays where it is
    int                  Through = 0, Stuck = 0;
};

inline bool Coarsen(const RdxMeshRound::FShape& S, const TList<FV3>& Place, const TList<int>& Corner, int Faces,
                    FCoarse& Out, const unsigned char* Skip = nullptr)
{
    using RdxMeshRound::FShape;
    const int NP = Place.Num;
    Out.Tri.Clear();
    Out.Region.Clear();
    Out.Through = Out.Stuck = 0;
    // Each place's kind - 0 a corner, 1 inside a flat polygon, 2 on a run - and for one on a run its two neighbours
    // along it: first the true edges at each place, how many and the first two.
    TList<int>           Count, RunA, RunB;
    TList<unsigned char> Kind, Used;
    if (!Count.Resize(NP) || !RunA.Resize(NP) || !RunB.Resize(NP) || !Kind.Resize(NP) || !Used.Resize(NP) ||
        !Out.Taken.Resize(NP) || !Out.Still.Resize(NP))
        return false;
    for (int v = 0; v < NP; v++)
    {
        Count[v] = 0;
        RunA[v] = RunB[v] = -1;
        Kind[v] = Used[v] = Out.Taken[v] = Out.Still[v] = 0;
    }
    for (int c = 0; c < Faces * 3; c++)
        if (!Skip || !Skip[c / 3])
            Used[S.Weld[Corner[c]]] = 1;
    for (int E = 0; E < S.Edges; E++)
    {
        if (S.EdgeKind[E] == FShape::Edge_Inner)
            continue;
        const int Ends[2] = { S.EdgeA[E], S.EdgeB[E] };
        for (int k = 0; k < 2; k++)
        {
            const int v = Ends[k];
            if (Count[v] == 0)
                RunA[v] = E;
            else if (Count[v] == 1)
                RunB[v] = E;
            Count[v]++;
        }
    }
    for (int v = 0; v < NP; v++)
    {
        const int Ea = RunA[v], Eb = RunB[v];
        RunA[v] = RunB[v] = -1;
        if (!Used[v] || S.Weld[v] != v)
            continue;
        if (Count[v] == 0)
        {
            Kind[v] = 1;
            continue;
        }
        if (Count[v] != 2 || S.EdgeKind[Ea] != S.EdgeKind[Eb])
            continue;
        const int    a  = (S.EdgeA[Ea] == v) ? S.EdgeB[Ea] : S.EdgeA[Ea];
        const int    b  = (S.EdgeA[Eb] == v) ? S.EdgeB[Eb] : S.EdgeA[Eb];
        const FV3    AB = Place[b] - Place[a];
        const double L2 = RdxSphere::Dot(AB, AB);
        if (!(L2 > 1e-12))
            continue;
        const double T = RdxSphere::Dot(Place[v] - Place[a], AB) / L2;
        if (!(T > 0.0 && T < 1.0) || Dist2(Place[a] + AB * T, Place[v]) > Straight * Straight)
            continue;
        Kind[v]      = 2;
        RunA[v]      = a;
        RunB[v]      = b;
        Out.Still[v] = S.EdgeKind[Ea] == FShape::Edge_Fixed;
    }

    // The triangles, and the live ones round each place: its corners threaded through NextAt, stale ones skipped.
    TList<int>           Tri, Head, NextAt, Star, Near;
    TList<unsigned char> Alive;
    TList<FV3>           Normal;
    if (!Tri.Resize(Faces * 3) || !NextAt.Resize(Faces * 3) || !Alive.Resize(Faces) || !Normal.Resize(Faces) ||
        !Head.Resize(NP))
        return false;
    for (int v = 0; v < NP; v++)
        Head[v] = -1;
    for (int c = 0; c < Faces * 3; c++)
    {
        Tri[c]       = S.Weld[Corner[c]];
        NextAt[c]    = Head[Tri[c]];
        Head[Tri[c]] = c;
    }
    for (int f = 0; f < Faces; f++)
    {
        Alive[f]  = (Skip && Skip[f]) ? 0 : 1;
        Normal[f] = FaceNormal(Place, Tri, f);
    }
    bool bMemory = true;
    // Place w taken onto its neighbour u: every live triangle round w that has u goes, every other takes u for w - where
    // each of those keeps its way round with some area.
    auto Collapse = [&](int w, int u) -> bool
    {
        Star.Clear();
        for (int c = Head[w]; c >= 0; c = NextAt[c])
            if (Alive[c / 3] && Tri[c] == w && !Star.Add(c))
            {
                bMemory = false;
                return false;
            }
        for (int i = 0; i < Star.Num; i++)
        {
            const int t = Star[i] / 3, k = Star[i] % 3;
            const int p = Tri[t * 3 + (k + 1) % 3], q = Tri[t * 3 + (k + 2) % 3];
            if (p != u && q != u && !KeepsWay(Place[u], Place[p], Place[q], Normal[t]))
                return false;
        }
        for (int i = 0; i < Star.Num; i++)
        {
            const int c = Star[i], t = c / 3, k = c % 3;
            if (Tri[t * 3 + (k + 1) % 3] == u || Tri[t * 3 + (k + 2) % 3] == u)
            {
                Alive[t] = 0;
                continue;
            }
            Tri[c]    = u;
            NextAt[c] = Head[u];
            Head[u]   = c;
        }
        Head[w] = -1;
        return true;
    };
    // Pass by pass, every place that may go: one on a run onto the nearer of its two neighbours along it, else the
    // other; one inside a flat polygon onto the nearest neighbour it can go onto.
    for (bool bAgain = true; bAgain && bMemory;)
    {
        bAgain = false;
        for (int w = 0; w < NP && bMemory; w++)
        {
            if (Kind[w] == 0 || Out.Taken[w])
                continue;
            int To = -1;
            if (Kind[w] == 2)
            {
                const int  a = RunA[w], b = RunB[w];
                const bool bA = Dist2(Place[a], Place[w]) <= Dist2(Place[b], Place[w]);
                if (Collapse(w, bA ? a : b))
                    To = bA ? a : b;
                else if (bMemory && Collapse(w, bA ? b : a))
                    To = bA ? b : a;
                if (To >= 0)
                {
                    // The run goes on past w: its other neighbour and To are neighbours along it now.
                    const int Other = (To == a) ? b : a;
                    if (RunA[Other] == w)
                        RunA[Other] = To;
                    else if (RunB[Other] == w)
                        RunB[Other] = To;
                    if (RunA[To] == w)
                        RunA[To] = Other;
                    else if (RunB[To] == w)
                        RunB[To] = Other;
                }
            }
            else
            {
                Near.Clear();
                for (int c = Head[w]; c >= 0 && bMemory; c = NextAt[c])
                    if (Alive[c / 3] && Tri[c] == w)
                        for (int j = 1; j < 3; j++)
                        {
                            const int n     = Tri[(c / 3) * 3 + (c % 3 + j) % 3];
                            bool      bHave = false;
                            for (int i = 0; i < Near.Num && !bHave; i++)
                                bHave = Near[i] == n;
                            if (!bHave && !Near.Add(n))
                                bMemory = false;
                        }
                // Nearest first (few: a sort by insertion).
                for (int i = 1; i < Near.Num; i++)
                    for (int j = i; j > 0 && Dist2(Place[Near[j]], Place[w]) < Dist2(Place[Near[j - 1]], Place[w]); j--)
                    {
                        const int Swap = Near[j];
                        Near[j]        = Near[j - 1];
                        Near[j - 1]    = Swap;
                    }
                for (int i = 0; i < Near.Num && To < 0 && bMemory; i++)
                    if (Collapse(w, Near[i]))
                        To = Near[i];
            }
            if (To >= 0)
            {
                Out.Taken[w] = 1;
                Out.Through++;
                bAgain = true;
            }
        }
    }
    if (!bMemory)
        return false;
    for (int w = 0; w < NP; w++)
        Out.Stuck += (Kind[w] != 0 && !Out.Taken[w]) ? 1 : 0;

    // Each flat polygon cut again by the Delaunay rule. A side's twin: the other live triangle's side the other way
    // along it, in the same flat polygon - none at the polygon's edge, or where more than one is.
    TList<int> Twin, Stack;
    if (!Twin.Resize(Faces * 3))
        return false;
    {
        FCells Sides;
        if (!Sides.Init(Faces * 3))
            return false;
        auto KeyOf = [&](int a, int b) { return (long long)a * (long long)NP + (long long)b; };
        for (int c = 0; c < Faces * 3; c++)
        {
            Twin[c] = -1;
            if (Alive[c / 3] && !Sides.Add(KeyOf(Tri[c], Tri[(c / 3) * 3 + (c % 3 + 1) % 3]), c))
                return false;
        }
        for (int c = 0; c < Faces * 3; c++)
        {
            if (!Alive[c / 3])
                continue;
            const long long K     = KeyOf(Tri[(c / 3) * 3 + (c % 3 + 1) % 3], Tri[c]);
            int             Found = -1, Many = 0;
            for (int e = Sides.Head[Sides.Bucket(K)]; e >= 0; e = Sides.Next[e])
                if (Sides.Key[e] == K)
                {
                    Found = Sides.Item[e];
                    Many++;
                }
            if (Many == 1 && S.Poly[Found / 3] == S.Poly[c / 3])
                Twin[c] = Found;
        }
    }
    for (int c = 0; c < Faces * 3; c++)
        if (Twin[c] > c && !Stack.Add(c))
            return false;
    for (int Budget = 32 * Faces + 64; Stack.Num > 0 && Budget > 0; Budget--)
    {
        const int h = Stack[--Stack.Num];
        const int g = (h >= 0) ? Twin[h] : -1;
        if (g < 0 || !Alive[h / 3] || !Alive[g / 3])
            continue;
        const int t = h / 3, k = h % 3, t2 = g / 3, k2 = g % 3;
        const int a = Tri[t * 3 + k], b = Tri[t * 3 + (k + 1) % 3], c = Tri[t * 3 + (k + 2) % 3];
        const int d = Tri[t2 * 3 + (k2 + 2) % 3];
        if (Tri[t2 * 3 + k2] != b || Tri[t2 * 3 + (k2 + 1) % 3] != a || c == d)
            continue;
        if (AngleAt(Place[c], Place[a], Place[b]) + AngleAt(Place[d], Place[b], Place[a]) <= RdxSphere::Pi + 1e-9)
            continue;
        // The two new ones must keep the way round: the four corners make a convex quad.
        if (!KeepsWay(Place[a], Place[d], Place[c], Normal[t]) || !KeepsWay(Place[d], Place[b], Place[c], Normal[t]))
            continue;
        const int X1 = Twin[t * 3 + (k + 1) % 3], X2 = Twin[t * 3 + (k + 2) % 3];
        const int Y1 = Twin[t2 * 3 + (k2 + 1) % 3], Y2 = Twin[t2 * 3 + (k2 + 2) % 3];
        Tri[t * 3]      = a;
        Tri[t * 3 + 1]  = d;
        Tri[t * 3 + 2]  = c;
        Tri[t2 * 3]     = d;
        Tri[t2 * 3 + 1] = b;
        Tri[t2 * 3 + 2] = c;
        auto Link = [&](int P, int Q)
        {
            Twin[P] = Q;
            if (Q >= 0)
                Twin[Q] = P;
        };
        Link(t * 3, Y1);
        Link(t * 3 + 1, t2 * 3 + 2);
        Link(t * 3 + 2, X2);
        Link(t2 * 3, Y2);
        Link(t2 * 3 + 1, X1);
        if (!Stack.Add(t * 3) || !Stack.Add(t * 3 + 2) || !Stack.Add(t2 * 3) || !Stack.Add(t2 * 3 + 1))
            return false;
    }
    for (int t = 0; t < Faces; t++)
        if (Alive[t] && (!Out.Tri.Add(Tri[t * 3]) || !Out.Tri.Add(Tri[t * 3 + 1]) || !Out.Tri.Add(Tri[t * 3 + 2]) ||
                         !Out.Region.Add(S.Poly[t])))
            return false;
    return true;
}

// The move of the point at (U, V) of a quad whose sides from its corner k to k + 1 are S[0..3] (corner 0 at U = V = 0,
// corner 1 at U = 1, corner 3 at V = 1): each side's curve where the point's lines across the quad meet it, the two
// pairs blended by how far across it lies - a Coons patch; the corners stay where they are.
inline RdxMeshRound::FV QuadMoveAt(const RdxMeshRound::FEdgeTerms* S, float U, float V)
{
    using RdxMeshRound::EdgeCurveAt;
    return EdgeCurveAt(S[0], U) * (1.0f - V) + EdgeCurveAt(S[2], 1.0f - U) * V + EdgeCurveAt(S[3], 1.0f - V) * (1.0f - U) +
           EdgeCurveAt(S[1], V) * U;
}

// Where point P lies in the flat quad of corners Q (the way round), as the share of the way from corner 0 to 1 (U) and
// from 0 to 3 (V) - in the plane laid flat along the axis Drop - its bilinear coordinates. False where it has none.
inline bool QuadUvOf(const FV3* Q, int Drop, const FV3& P, double& U, double& V)
{
    auto X = [&](const FV3& A) { return (Drop == 0) ? A.Y : A.X; };
    auto Y = [&](const FV3& A) { return (Drop == 2) ? A.Y : A.Z; };
    const double Ex = X(Q[1]) - X(Q[0]), Ey = Y(Q[1]) - Y(Q[0]);
    const double Fx = X(Q[3]) - X(Q[0]), Fy = Y(Q[3]) - Y(Q[0]);
    const double Gx = X(Q[0]) - X(Q[1]) + X(Q[2]) - X(Q[3]), Gy = Y(Q[0]) - Y(Q[1]) + Y(Q[2]) - Y(Q[3]);
    const double Hx = X(P) - X(Q[0]), Hy = Y(P) - Y(Q[0]);
    const double K2 = Gx * Fy - Gy * Fx, K1 = Ex * Fy - Ey * Fx + Hx * Gy - Hy * Gx, K0 = Hx * Ey - Hy * Ex;
    if (!(fabs(Ex * Fy - Ey * Fx) > 1e-12))
        return false;
    // U from V by the way along which the quad's side grows the most.
    auto UOf = [&](double Vv)
    {
        const double Dx = Ex + Gx * Vv, Dy = Ey + Gy * Vv;
        return (fabs(Dx) >= fabs(Dy)) ? (Hx - Fx * Vv) / Dx : (Hy - Fy * Vv) / Dy;
    };
    // K2 V^2 + K1 V + K0 = 0, its roots taken without the difference of two near numbers: q = -(K1 + sign K1 root) / 2,
    // the roots q / K2 and K0 / q - the second the one a parallelogram (K2 = 0) has. The root whose U and V both lie in
    // the quad, else the nearer.
    const double W = K1 * K1 - 4.0 * K0 * K2;
    if (W < 0.0)
        return false;
    const double Qr = -0.5 * (K1 + ((K1 < 0.0) ? -sqrt(W) : sqrt(W)));
    if (Qr == 0.0)
    {
        if (K2 == 0.0)
            return false;
        V = 0.0;
        U = UOf(V);
        return true;
    }
    const double V1 = K0 / Qr, V2 = (K2 != 0.0) ? Qr / K2 : 1e30;
    auto         Off = [&](double Vv, double Uu) { return MaxOf(MaxOf(-Vv, Vv - 1.0), MaxOf(-Uu, Uu - 1.0)); };
    const double U1 = UOf(V1), U2 = (V2 < 1e29) ? UOf(V2) : 1e30;
    if (Off(V1, U1) <= Off(V2, U2))
    {
        U = U1;
        V = V1;
    }
    else
    {
        U = U2;
        V = V2;
    }
    return true;
}

// The level's surfaces rounded (Round's step 6): each piece of the flat polygons as one (FCoarse) - its first corner,
// its two sides from it, its box - and its edges' terms in the shape made of the pieces, and the pieces by flat polygon:
// the move of any point of the level's surfaces (MoveAt).
struct FSurface
{
    TList<FV3>                      A, E1, E2, Lo, Hi;
    TList<double>                   D00, D01, D11, Inv;
    TList<RdxMeshRound::FEdgeTerms> Terms;   // three a piece, from its corner k to k + 1
    // By flat polygon (its lowest fine triangle): its pieces, the one a point was last found on, and whether any of
    // their edges bends.
    TList<int>           First, Count, Items, Last;
    TList<unsigned char> Bends;
    // By flat polygon: its quad (QuadsOf), -1 none; a quad's four corners the way round, its four sides from corner k
    // to k + 1, and the axis its plane is laid flat along.
    TList<int>                      QuadOf;
    TList<FV3>                      QuadCorner;
    TList<RdxMeshRound::FEdgeTerms> QuadSide;
    TList<int>                      QuadDrop;
    float                           Cap = 8.0f;
    // Each piece's edges' and each quad's sides' numbers in the pieces' shape: one edge, one curve, on both its sides.
    TList<int>                      TermEdge, QuadEdge;

    // The piece of flat polygon R that point Q lies on, with its corners' shares there (B): the piece last found first,
    // then the others by their boxes, then the one it lies nearest by its shares. -1: R has none.
    int PieceAt(int R, const FV3& Q, double* B)
    {
        if (R < 0 || R >= Count.Num || Count[R] <= 0)
            return -1;
        int    Best  = -1;
        double Least = -1e300;
        auto   Try   = [&](int i)
        {
            const FV3    D   = Q - A[i];
            const double D20 = RdxSphere::Dot(D, E1[i]), D21 = RdxSphere::Dot(D, E2[i]);
            const double B1 = (D11[i] * D20 - D01[i] * D21) * Inv[i], B2 = (D00[i] * D21 - D01[i] * D20) * Inv[i];
            const double B0 = 1.0 - B1 - B2;
            const double L  = MinOf(B0, MinOf(B1, B2));
            if (L > Least)
            {
                Least = L;
                Best  = i;
                B[0]  = B0;
                B[1]  = B1;
                B[2]  = B2;
            }
            return L >= -1e-6;
        };
        bool bOn = Last[R] >= 0 && Try(Last[R]);
        for (int Pass = 0; Pass < 2 && !bOn; Pass++)
            for (int j = First[R]; j < First[R] + Count[R] && !bOn; j++)
            {
                const int i = Items[j];
                if (Pass == 0 && (Q.X < Lo[i].X || Q.Y < Lo[i].Y || Q.Z < Lo[i].Z || Q.X > Hi[i].X || Q.Y > Hi[i].Y ||
                                  Q.Z > Hi[i].Z))
                    continue;
                bOn = Try(i);
            }
        Last[R] = Best;
        return Best;
    }

    // How point Q of flat polygon R moves: as the piece it lies on moves its point there (MeshRound.h's PatchMoveAt),
    // held to the cap.
    RdxMeshRound::FV MoveAt(int R, const FV3& Q)
    {
        // A quad: its Coons patch, at the point's place in it held within it.
        if (R >= 0 && R < QuadOf.Num && QuadOf[R] >= 0 && Bends[R])
        {
            const int q = QuadOf[R];
            double    U = 0.0, V = 0.0;
            if (QuadUvOf(&QuadCorner[q * 4], QuadDrop[q], Q, U, V))
            {
                RdxMeshRound::FV M = QuadMoveAt(&QuadSide[q * 4], (float)MinOf(MaxOf(U, 0.0), 1.0),
                                                (float)MinOf(MaxOf(V, 0.0), 1.0));
                const float      L = RdxMeshRound::Length(M);
                return (L > Cap) ? M * (Cap / L) : M;
            }
        }
        double    B[3] = { 0.0, 0.0, 0.0 };
        const int i    = (R >= 0 && R < Bends.Num && Bends[R]) ? PieceAt(R, Q, B) : -1;
        if (i < 0)
            return RdxMeshRound::V(0.0f, 0.0f, 0.0f);
        const double B0 = MaxOf(B[0], 0.0), B1 = MaxOf(B[1], 0.0), B2 = MaxOf(B[2], 0.0), Sum = B0 + B1 + B2;
        RdxMeshRound::FV M = RdxMeshRound::PatchMoveAt(&Terms[i * 3], (float)(B0 / Sum), (float)(B1 / Sum),
                                                       (float)(B2 / Sum));
        const float L = RdxMeshRound::Length(M);
        return (L > Cap) ? M * (Cap / L) : M;
    }
};

// The surface of the pieces C, from the shape made of them (CS, in the level's pose): Regions, how many flat polygons
// there may be (the fine triangles' count). The edges' terms worked out ahead and each piece made on threads (Workers),
// what they add up to by flat polygon after, in the pieces' order.
inline bool SurfaceOf(RdxMeshRound::FShape& CS, const FCoarse& C, const TList<FV3>& Place, int Regions, float Cap,
                      FSurface& Out, int Workers)
{
    const int N = C.Region.Num;
    Out.Cap     = Cap;
    TList<unsigned char> PieceBends;
    if (!Out.A.Resize(N) || !Out.E1.Resize(N) || !Out.E2.Resize(N) || !Out.Lo.Resize(N) || !Out.Hi.Resize(N) ||
        !Out.D00.Resize(N) || !Out.D01.Resize(N) || !Out.D11.Resize(N) || !Out.Inv.Resize(N) ||
        !Out.Terms.Resize(N * 3) || !Out.TermEdge.Resize(N * 3) || !Out.Items.Resize(N) || !Out.First.Resize(Regions) ||
        !Out.Count.Resize(Regions) ||
        !Out.Last.Resize(Regions) || !Out.Bends.Resize(Regions) || !PieceBends.Resize(N))
        return false;
    for (int r = 0; r < Regions; r++)
    {
        Out.First[r] = Out.Count[r] = 0;
        Out.Last[r]  = -1;
        Out.Bends[r] = 0;
    }
    CS.TermsAll(Workers);
    RdxSphere::OnThreads(N, 256, Workers, [&](int i, int)
    {
        const FV3 P0 = Place[C.Tri[i * 3]], P1 = Place[C.Tri[i * 3 + 1]], P2 = Place[C.Tri[i * 3 + 2]];
        Out.A[i]          = P0;
        Out.E1[i]         = P1 - P0;
        Out.E2[i]         = P2 - P0;
        Out.D00[i]        = RdxSphere::Dot(Out.E1[i], Out.E1[i]);
        Out.D01[i]        = RdxSphere::Dot(Out.E1[i], Out.E2[i]);
        Out.D11[i]        = RdxSphere::Dot(Out.E2[i], Out.E2[i]);
        const double Den  = Out.D00[i] * Out.D11[i] - Out.D01[i] * Out.D01[i];
        Out.Inv[i]        = (fabs(Den) > 1e-18) ? 1.0 / Den : 0.0;
        Out.Lo[i]         = RdxSphere::V3(MinOf(P0.X, MinOf(P1.X, P2.X)) - 0.01, MinOf(P0.Y, MinOf(P1.Y, P2.Y)) - 0.01,
                                          MinOf(P0.Z, MinOf(P1.Z, P2.Z)) - 0.01);
        Out.Hi[i]         = RdxSphere::V3(MaxOf(P0.X, MaxOf(P1.X, P2.X)) + 0.01, MaxOf(P0.Y, MaxOf(P1.Y, P2.Y)) + 0.01,
                                          MaxOf(P0.Z, MaxOf(P1.Z, P2.Z)) + 0.01);
        PieceBends[i]     = 0;
        for (int e = 0; e < 3; e++)
        {
            Out.Terms[i * 3 + e]    = CS.EdgeTerms(i, e);
            Out.TermEdge[i * 3 + e] = CS.EdgeOf[i * 3 + e];
            for (int k = 0; k < RdxMeshRound::EDGE_SLOTS; k++)
                if (RdxMeshRound::Length(Out.Terms[i * 3 + e].T[k]) > 1e-6f)
                    PieceBends[i] = 1;
        }
    });
    for (int i = 0; i < N; i++)
    {
        const int R = C.Region[i];
        if (PieceBends[i])
            Out.Bends[R] = 1;
        Out.Count[R]++;
    }
    for (int r = 0, Next = 0; r < Regions; r++)
    {
        Out.First[r] = Next;
        Next += Out.Count[r];
        Out.Count[r] = 0;
    }
    for (int i = 0; i < N; i++)
    {
        const int R                               = C.Region[i];
        Out.Items[Out.First[R] + Out.Count[R]++] = i;
    }
    return true;
}

// The flat polygons that are convex quads of four sides, each one edge of the pieces' shape CS (FSurface's QuadOf): a
// flat polygon's outline is the sides of its pieces that none of its other pieces has the other way along; four of
// them in one ring, turning the same way at every corner, make a quad. Only one that bends is kept.
inline bool QuadsOf(RdxMeshRound::FShape& CS, const FCoarse& C, const TList<FV3>& Place, FSurface& Out)
{
    const int N = C.Region.Num, NP = Place.Num, Regions = Out.First.Num;
    Out.QuadCorner.Clear();
    Out.QuadSide.Clear();
    Out.QuadDrop.Clear();
    Out.QuadEdge.Clear();
    TList<int> OutCount, OutSide;
    if (!Out.QuadOf.Resize(Regions) || !OutCount.Resize(Regions) || !OutSide.Resize(Regions * 4))
        return false;
    for (int r = 0; r < Regions; r++)
        Out.QuadOf[r] = OutCount[r] = 0;
    for (int r = 0; r < Regions; r++)
        Out.QuadOf[r] = -1;
    FCells Sides;
    if (!Sides.Init(N * 3))
        return false;
    auto KeyOf = [&](int a, int b) { return (long long)a * (long long)NP + (long long)b; };
    auto EndOf = [&](int c) { return C.Tri[(c / 3) * 3 + (c % 3 + 1) % 3]; };
    for (int c = 0; c < N * 3; c++)
        if (!Sides.Add(KeyOf(C.Tri[c], EndOf(c)), c))
            return false;
    for (int c = 0; c < N * 3; c++)
    {
        const int       R = C.Region[c / 3];
        const long long K = KeyOf(EndOf(c), C.Tri[c]);
        bool            bInner = false;
        for (int e = Sides.Head[Sides.Bucket(K)]; e >= 0 && !bInner; e = Sides.Next[e])
            bInner = Sides.Key[e] == K && C.Region[Sides.Item[e] / 3] == R;
        if (bInner)
            continue;
        if (OutCount[R] < 4)
            OutSide[R * 4 + OutCount[R]] = c;
        OutCount[R]++;
    }
    for (int r = 0; r < Regions; r++)
    {
        if (OutCount[r] != 4 || !Out.Bends[r] || Out.Count[r] <= 0)
            continue;
        // In one ring: each side starting where the one before it ends.
        int  S[4] = { OutSide[r * 4], -1, -1, -1 };
        bool bRing = true;
        for (int k = 1; k < 4 && bRing; k++)
        {
            for (int j = 1; j < 4 && S[k] < 0; j++)
                if (C.Tri[OutSide[r * 4 + j]] == EndOf(S[k - 1]))
                    S[k] = OutSide[r * 4 + j];
            bRing = S[k] >= 0;
        }
        if (!bRing || EndOf(S[3]) != C.Tri[S[0]])
            continue;
        FV3 Q[4];
        for (int k = 0; k < 4; k++)
            Q[k] = Place[C.Tri[S[k]]];
        const int i0 = Out.Items[Out.First[r]];
        const FV3 Nr = RdxSphere::Cross(Out.E1[i0], Out.E2[i0]);
        bool      bConvex = RdxSphere::Dot(Nr, Nr) > 1e-18;
        for (int k = 0; k < 4 && bConvex; k++)
            bConvex = KeepsWay(Q[k], Q[(k + 1) % 4], Q[(k + 2) % 4], Nr * (1.0 / RdxSphere::Length(Nr)));
        if (!bConvex)
            continue;
        Out.QuadOf[r] = Out.QuadDrop.Num;
        const double AX = fabs(Nr.X), AY = fabs(Nr.Y), AZ = fabs(Nr.Z);
        if (!Out.QuadDrop.Add((AX >= AY && AX >= AZ) ? 0 : (AY >= AZ) ? 1 : 2))
            return false;
        for (int k = 0; k < 4; k++)
            if (!Out.QuadCorner.Add(Q[k]) || !Out.QuadSide.Add(CS.EdgeTerms(S[k] / 3, S[k] % 3)) ||
                !Out.QuadEdge.Add(CS.EdgeOf[S[k]]))
                return false;
    }
    return true;
}

// The surface's pieces that would fold over - a part of one turned over against its flat place, where it does not only
// move along its plane - unbent (Round's step 5): round by round, every edge of such a piece, or side of such a quad, has
// its bends halved, and what those edges bound is looked at again. The edges' terms are what a piece moves by on both
// sides of each edge, so the surface stays whole, and the card's copy of it (FaceTerm) is made of the same terms.
// A piece is looked at on a grid of UnfoldGrid steps a side; a triangle whose bends are too small against its narrowest
// height for any part of it to turn - its move's slope stays under one: each edge's curve at most the sum of its bends,
// weighed in by its corners' cubes, and the middle's half of all of them, (22 / 9 + 1 / 6) times the sum of its bends
// over that height at the most - is not. Unbent: the edges whose bends were halved, every time.
static const int    UnfoldGrid   = 8;
static const int    UnfoldRounds = 8;
static const double UnfoldSlope  = 2.7;
inline bool UnfoldSurface(FSurface& S, int Edges, int& Unbent, int Workers)
{
    using RdxMeshRound::FV;
    Unbent       = 0;
    const int NP = S.A.Num, NQ = S.QuadDrop.Num, NR = S.First.Num;
    if (Edges <= 0 || (NP <= 0 && NQ <= 0))
        return true;
    TList<float>                    Scale;
    TList<unsigned char>            Hit, Dirty, DirtyQuad;
    TList<RdxMeshRound::FEdgeTerms> TermsMade, SidesMade;
    TList<int>                      RegionOfPiece;
    TList<int>                      UseFirst, UseCount, Uses;   // an edge: the pieces (i) and quads (NP + q) it bounds
    if (!Scale.Resize(Edges) || !Hit.Resize(Edges) || !Dirty.Resize(NP) || !DirtyQuad.Resize(NQ) ||
        !TermsMade.Resize(NP * 3) || !SidesMade.Resize(NQ * 4) || !RegionOfPiece.Resize(NP) || !UseFirst.Resize(Edges) ||
        !UseCount.Resize(Edges) || !Uses.Resize(NP * 3 + NQ * 4))
        return false;
    for (int e = 0; e < Edges; e++)
    {
        Scale[e]    = 1.0f;
        UseCount[e] = 0;
    }
    for (int i = 0; i < NP * 3; i++)
        TermsMade[i] = S.Terms[i];
    for (int i = 0; i < NQ * 4; i++)
        SidesMade[i] = S.QuadSide[i];
    for (int i = 0; i < NP; i++)
        RegionOfPiece[i] = -1;
    for (int r = 0; r < NR; r++)
        for (int j = S.First[r]; j < S.First[r] + S.Count[r]; j++)
            RegionOfPiece[S.Items[j]] = r;
    for (int i = 0; i < NP * 3; i++)
        UseCount[S.TermEdge[i]]++;
    for (int i = 0; i < NQ * 4; i++)
        UseCount[S.QuadEdge[i]]++;
    for (int e = 0, Next = 0; e < Edges; e++)
    {
        UseFirst[e] = Next;
        Next += UseCount[e];
        UseCount[e] = 0;
    }
    for (int i = 0; i < NP * 3; i++)
        Uses[UseFirst[S.TermEdge[i]] + UseCount[S.TermEdge[i]]++] = i / 3;
    for (int i = 0; i < NQ * 4; i++)
        Uses[UseFirst[S.QuadEdge[i]] + UseCount[S.QuadEdge[i]]++] = NP + i / 4;
    for (int i = 0; i < NP; i++)
        Dirty[i] = 1;
    for (int q = 0; q < NQ; q++)
        DirtyQuad[q] = 1;

    const double InPlane = 0.05;
    enum { G = UnfoldGrid };
    auto Capped = [&](const FV& M)
    {
        const float L = RdxMeshRound::Length(M);
        return (L > S.Cap) ? M * (S.Cap / L) : M;
    };
    // The grid's triangle a, b, c turned over against its flat place, and not only moved along the plane of normal Nu:
    // the grid's flat places and their moves in Flat and Move, the looker's own.
    auto Folds = [&](const FV3* Flat, const FV* Move, int a, int b, int c, const FV3& Nu) -> bool
    {
        const FV3    Ma = RdxSphere::V3(Move[a].X, Move[a].Y, Move[a].Z), Mb = RdxSphere::V3(Move[b].X, Move[b].Y, Move[b].Z),
                     Mc = RdxSphere::V3(Move[c].X, Move[c].Y, Move[c].Z);
        const FV3    Nflat = RdxSphere::Cross(Flat[b] - Flat[a], Flat[c] - Flat[a]);
        if (!(RdxSphere::Length(Nflat) > 1e-9))
            return false;
        const FV3 N = RdxSphere::Cross((Flat[b] + Mb) - (Flat[a] + Ma), (Flat[c] + Mc) - (Flat[a] + Ma));
        if (RdxSphere::Dot(N, Nflat) >= 0.0)
            return false;
        return fabs(RdxSphere::Dot(Ma, Nu)) > InPlane || fabs(RdxSphere::Dot(Mb, Nu)) > InPlane ||
               fabs(RdxSphere::Dot(Mc, Nu)) > InPlane;
    };
    auto QuadFolds = [&](int q, FV3* Flat, FV* Move) -> bool
    {
        const FV3*   Q  = &S.QuadCorner[q * 4];
        FV3          Nu = RdxSphere::Cross(Q[1] - Q[0], Q[3] - Q[0]);
        const double Ln = RdxSphere::Length(Nu);
        if (!(Ln > 1e-12))
            return false;
        Nu = Nu * (1.0 / Ln);
        for (int i = 0; i <= G; i++)
            for (int j = 0; j <= G; j++)
            {
                const double U = (double)i / G, V = (double)j / G;
                Flat[i * (G + 1) + j] =
                    Q[0] * ((1.0 - U) * (1.0 - V)) + Q[1] * (U * (1.0 - V)) + Q[2] * (U * V) + Q[3] * ((1.0 - U) * V);
                Move[i * (G + 1) + j] = Capped(QuadMoveAt(&S.QuadSide[q * 4], (float)U, (float)V));
            }
        for (int i = 0; i < G; i++)
            for (int j = 0; j < G; j++)
            {
                const int a = i * (G + 1) + j, b = (i + 1) * (G + 1) + j, c = (i + 1) * (G + 1) + j + 1,
                          d = i * (G + 1) + j + 1;
                if (Folds(Flat, Move, a, b, c, Nu) || Folds(Flat, Move, a, c, d, Nu))
                    return true;
            }
        return false;
    };
    auto PieceFolds = [&](int i, FV3* Flat, FV* Move) -> bool
    {
        FV3          Nu = RdxSphere::Cross(S.E1[i], S.E2[i]);
        const double Ln = RdxSphere::Length(Nu);
        if (!(Ln > 1e-12))
            return false;
        Nu = Nu * (1.0 / Ln);
        // Its narrowest height, and the sum of its bends: too small for the slope to reach one, nothing turns.
        const double Longest = sqrt(MaxOf(S.D00[i], MaxOf(S.D11[i], RdxSphere::Dot(S.E2[i] - S.E1[i], S.E2[i] - S.E1[i]))));
        const double Height  = (Longest > 0.0) ? Ln / Longest : 0.0;
        double       Bends   = 0.0;
        for (int e = 0; e < 3; e++)
            for (int k = 0; k < RdxMeshRound::EDGE_SLOTS; k++)
                Bends += RdxMeshRound::Length(S.Terms[i * 3 + e].T[k]);
        if (UnfoldSlope * Bends < Height)
            return false;
        int At[G + 1][G + 1];
        int n = 0;
        for (int a = 0; a <= G; a++)
            for (int b = 0; a + b <= G; b++)
            {
                const double B1 = (double)a / G, B2 = (double)b / G, B0 = 1.0 - B1 - B2;
                Flat[n]  = S.A[i] + S.E1[i] * B1 + S.E2[i] * B2;
                Move[n]  = Capped(RdxMeshRound::PatchMoveAt(&S.Terms[i * 3], (float)B0, (float)B1, (float)B2));
                At[a][b] = n++;
            }
        for (int a = 0; a < G; a++)
            for (int b = 0; a + b < G; b++)
                if (Folds(Flat, Move, At[a][b], At[a + 1][b], At[a][b + 1], Nu) ||
                    (a + b + 1 < G && Folds(Flat, Move, At[a + 1][b], At[a + 1][b + 1], At[a][b + 1], Nu)))
                    return true;
        return false;
    };
    // The pieces (i) and quads (NP + q) a round looks at, and which of them fold: looked at on threads, each on a grid of
    // its own, the surface only read; what they hit taken in their order after.
    TList<int>           Look;
    TList<unsigned char> Folded;
    for (int Round = 0; Round < UnfoldRounds; Round++)
    {
        bool bAny = false;
        for (int e = 0; e < Edges; e++)
            Hit[e] = 0;
        // A quad region is moved by its quad alone (MoveAt): its pieces are not looked at.
        Look.Clear();
        for (int i = 0; i < NP; i++)
        {
            const int r = RegionOfPiece[i];
            if (!Dirty[i] || r < 0 || !S.Bends[r] || (r < S.QuadOf.Num && S.QuadOf[r] >= 0))
                continue;
            if (!Look.Add(i))
                return false;
        }
        for (int q = 0; q < NQ; q++)
            if (DirtyQuad[q] && !Look.Add(NP + q))
                return false;
        if (!Folded.Resize(Look.Num))
            return false;
        RdxSphere::OnThreads(Look.Num, 64, Workers, [&](int k, int)
        {
            FV3       Flat[(G + 1) * (G + 1)];
            FV        Move[(G + 1) * (G + 1)];
            const int j = Look[k];
            Folded[k]   = ((j < NP) ? PieceFolds(j, Flat, Move) : QuadFolds(j - NP, Flat, Move)) ? 1 : 0;
        });
        for (int k = 0; k < Look.Num; k++)
        {
            if (!Folded[k])
                continue;
            bAny        = true;
            const int j = Look[k];
            if (j < NP)
            {
                for (int e = 0; e < 3; e++)
                    Hit[S.TermEdge[j * 3 + e]] = 1;
#ifdef LEVELROUND_TRACE
                LevelRoundTraceFold(Round, S.A[j], S.A[j] + S.E1[j], S.A[j] + S.E2[j]);
#endif
                continue;
            }
            const int q = j - NP;
            for (int e = 0; e < 4; e++)
                Hit[S.QuadEdge[q * 4 + e]] = 1;
#ifdef LEVELROUND_TRACE
            LevelRoundTraceFold(Round, S.QuadCorner[q * 4], S.QuadCorner[q * 4 + 1], S.QuadCorner[q * 4 + 2]);
#endif
        }
        for (int i = 0; i < NP; i++)
            Dirty[i] = 0;
        for (int q = 0; q < NQ; q++)
            DirtyQuad[q] = 0;
        if (!bAny)
            break;
        for (int e = 0; e < Edges; e++)
        {
            if (!Hit[e])
                continue;
            Scale[e] *= 0.5f;
            Unbent++;
            for (int u = UseFirst[e]; u < UseFirst[e] + UseCount[e]; u++)
            {
                if (Uses[u] < NP)
                    Dirty[Uses[u]] = 1;
                else
                    DirtyQuad[Uses[u] - NP] = 1;
            }
        }
        for (int i = 0; i < NP * 3; i++)
            for (int k = 0; k < RdxMeshRound::EDGE_SLOTS; k++)
                S.Terms[i].T[k] = TermsMade[i].T[k] * Scale[S.TermEdge[i]];
        for (int i = 0; i < NQ * 4; i++)
            for (int k = 0; k < RdxMeshRound::EDGE_SLOTS; k++)
                S.QuadSide[i].T[k] = SidesMade[i].T[k] * Scale[S.QuadEdge[i]];
    }
    return true;
}

// Whether the triangle T and the convex ring of n places R, lying in one plane of normal N, overlap by more than a hair:
// no side of either parts them, laid flat along the axis N is most along.
inline bool Overlaps(const FV3* T, const FV3* R, int n, const FV3& N)
{
    const double AX = fabs(N.X), AY = fabs(N.Y), AZ = fabs(N.Z);
    const int    Drop = (AX >= AY && AX >= AZ) ? 0 : (AY >= AZ) ? 1 : 2;
    auto         U    = [&](const FV3& P) { return (Drop == 0) ? P.Y : P.X; };
    auto         W    = [&](const FV3& P) { return (Drop == 2) ? P.Y : P.Z; };
    auto         Parts = [&](const FV3& P, const FV3& Q)
    {
        double       Ax = W(P) - W(Q), Ay = U(Q) - U(P);
        const double L  = sqrt(Ax * Ax + Ay * Ay);
        if (!(L > 1e-9))
            return false;
        Ax /= L;
        Ay /= L;
        double T0 = 1e300, T1 = -1e300, R0 = 1e300, R1 = -1e300;
        for (int k = 0; k < 3; k++)
        {
            const double D = U(T[k]) * Ax + W(T[k]) * Ay;
            T0             = MinOf(T0, D);
            T1             = MaxOf(T1, D);
        }
        for (int k = 0; k < n; k++)
        {
            const double D = U(R[k]) * Ax + W(R[k]) * Ay;
            R0             = MinOf(R0, D);
            R1             = MaxOf(R1, D);
        }
        return T1 <= R0 + 1e-3 || R1 <= T0 + 1e-3;
    };
    for (int k = 0; k < 3; k++)
        if (Parts(T[k], T[(k + 1) % 3]))
            return false;
    for (int k = 0; k < n; k++)
        if (Parts(R[k], R[(k + 1) % n]))
            return false;
    return true;
}

}   // namespace Detail

// The polygons' sides along soft edges, for the light maps' blending over them (FResult's LightSides): each side of a
// polygon's own - from one of its own corners to the next, the T-junctions' places between - with its runs that are
// soft edges of the shape S, each with the polygon across. How far into the polygon the light across is taken in: half
// its flat polygon's depth from the side - as far as a surface rounded from side to side turns back to the polygon's own
// way - but no further than a third of the room a bend held to Cap takes at that turn (MeshRound.h's RoomOf), where the
// bend has turned back.
inline bool LightSidesOf(const RdxMeshRound::FShape& S, const TList<FV3>& Place, const TList<int>& Corner,
                         const TList<int>& FaceOf, int Faces, const TList<int>& Ring, const TList<unsigned char>& RingOwn,
                         const TList<int>& RingFirst, const TList<int>& RingCount, const TList<int>& PolyFace0,
                         const TList<int>& PolyFaces, int NumPolys, double Cap, FResult& Out)
{
    using namespace Detail;
    if (!Out.PolySideFirst.Resize(NumPolys) || !Out.PolySideCount.Resize(NumPolys))
        return false;
    // Each flat polygon's triangles together, by its lowest (FShape::Poly).
    TList<int> RegionFirst, RegionCount, RegionFaces;
    if (!RegionFirst.Resize(Faces) || !RegionCount.Resize(Faces) || !RegionFaces.Resize(Faces))
        return false;
    for (int f = 0; f < Faces; f++)
        RegionCount[f] = 0;
    for (int f = 0; f < Faces; f++)
        RegionCount[S.Poly[f]]++;
    for (int f = 0, Next = 0; f < Faces; f++)
    {
        RegionFirst[f] = Next;
        Next += RegionCount[f];
        RegionCount[f] = 0;
    }
    for (int f = 0; f < Faces; f++)
    {
        const int r                                = S.Poly[f];
        RegionFaces[RegionFirst[r] + RegionCount[r]++] = f;
    }
    TList<int> SoftAt, SoftFace, SoftEdge;
    for (int p = 0; p < NumPolys; p++)
    {
        Out.PolySideFirst[p] = Out.LightSides.Num;
        Out.PolySideCount[p] = 0;
        const int n = RingCount[p];
        if (n < 3 || PolyFaces[p] <= 0)
            continue;
        const int* const           R   = &Ring[RingFirst[p]];
        const unsigned char* const Own = &RingOwn[RingFirst[p]];
        // Its triangles' soft edges that lie on its ring: where along the ring each starts.
        SoftAt.Clear();
        SoftFace.Clear();
        SoftEdge.Clear();
        for (int f = PolyFace0[p]; f < PolyFace0[p] + PolyFaces[p]; f++)
            for (int e = 0; e < 3; e++)
            {
                const int E = S.EdgeOf[f * 3 + e];
                if (S.EdgeKind[E] != RdxMeshRound::FShape::Edge_Soft || S.EdgeCount[E] != 2)
                    continue;
                const int a = Corner[f * 3 + e], b = Corner[f * 3 + (e + 1) % 3];
                int       At = -1;
                for (int i = 0; i < n && At < 0; i++)
                    if ((R[i] == a && R[(i + 1) % n] == b) || (R[i] == b && R[(i + 1) % n] == a))
                        At = i;
                if (At >= 0 && (!SoftAt.Add(At) || !SoftFace.Add(f) || !SoftEdge.Add(E)))
                    return false;
            }
        // In order round the ring (few: a sort by insertion).
        for (int a = 1; a < SoftAt.Num; a++)
            for (int b = a; b > 0 && SoftAt[b] < SoftAt[b - 1]; b--)
            {
                int T          = SoftAt[b];
                SoftAt[b]      = SoftAt[b - 1];
                SoftAt[b - 1]  = T;
                T              = SoftFace[b];
                SoftFace[b]    = SoftFace[b - 1];
                SoftFace[b - 1] = T;
                T              = SoftEdge[b];
                SoftEdge[b]    = SoftEdge[b - 1];
                SoftEdge[b - 1] = T;
            }
        for (int s = 0; s < SoftAt.Num;)
        {
            // The side the run lies on: from the polygon's own corner at or before it to the next.
            int S0 = SoftAt[s];
            while (S0 > 0 && !Own[S0])
                S0--;
            int S1 = (SoftAt[s] + 1) % n;
            while (S1 != S0 && !Own[S1])
                S1 = (S1 + 1) % n;
            int Last = s + 1;
            while (Last < SoftAt.Num && SoftAt[Last] < ((S1 > S0) ? S1 : n))
                Last++;
            const FV3    A = Place[R[S0]], B = Place[R[S1]];
            const double Long = RdxSphere::Length(B - A);
            const FV3    NP   = FaceNormal(Place, Corner, SoftFace[s]);
            const FV3    Into = IntoFace(Place, Corner, SoftFace[s], R[S0], R[S1], NP);
            if (Long > 1e-6 && RdxSphere::Dot(Into, Into) > 0.5)
            {
                const FV3  Dir = (B - A) * (1.0 / Long);
                FLightSide Side;
                Side.A         = A;
                Side.B         = B;
                Side.Into      = Into;
                Side.PartFirst = Out.LightParts.Num;
                for (int k = s; k < Last; k++)
                {
                    const int f = SoftFace[k], E = SoftEdge[k];
                    const int a = R[SoftAt[k]], b = R[(SoftAt[k] + 1) % n];
                    const int g = (S.EdgeC0[E] / 3 == f) ? S.EdgeC1[E] / 3 : S.EdgeC0[E] / 3;
                    const int q = FaceOf[g];
                    const FV3 NQ = FaceNormal(Place, Corner, g);
                    FLightPart Part;
                    Part.T0    = RdxSphere::Dot(Place[a] - A, Dir);
                    Part.T1    = RdxSphere::Dot(Place[b] - A, Dir);
                    Part.Other = q;
                    Part.Into  = IntoFace(Place, Corner, g, a, b, NQ);
                    if (Part.T1 < Part.T0 || RdxSphere::Dot(Part.Into, Part.Into) < 0.5)
                        continue;
                    // How far the polygon across reaches from the side: its own corners, or the triangle's.
                    Part.Depth = 0.0;
                    if (RingCount[q] >= 3)
                        for (int i = 0; i < RingCount[q]; i++)
                            Part.Depth = MaxOf(Part.Depth, RdxSphere::Dot(Place[Ring[RingFirst[q] + i]] - A, Part.Into));
                    else
                        for (int i = 0; i < 3; i++)
                            Part.Depth = MaxOf(Part.Depth, RdxSphere::Dot(Place[Corner[g * 3 + i]] - A, Part.Into));
                    // The turn between the two, and the reach into this side's flat polygon.
                    const double Cos  = MinOf(1.0, MaxOf(-1.0, RdxSphere::Dot(NP, NQ)));
                    const double Half = sin(0.5 * acos(Cos));
                    const double Most = (Half > 1e-6) ? Cap / ((double)RdxMeshRound::BendTop * Half) / 3.0 : 1e30;
                    double       Deep = 0.0;
                    const int    r    = S.Poly[f];
                    for (int i = RegionFirst[r]; i < RegionFirst[r] + RegionCount[r] && Deep < 2.0 * Most; i++)
                        for (int c = 0; c < 3; c++)
                            Deep = MaxOf(Deep, RdxSphere::Dot(Place[Corner[RegionFaces[i] * 3 + c]] - A, Into));
                    Part.Reach = MinOf(0.5 * Deep, Most);
                    if (Part.Reach > 1e-3 && !Out.LightParts.Add(Part))
                        return false;
                }
                Side.PartCount = Out.LightParts.Num - Side.PartFirst;
                if (Side.PartCount > 0)
                {
                    if (!Out.LightSides.Add(Side))
                        return false;
                    Out.PolySideCount[p]++;
                }
            }
            s = Last;
        }
    }
    return true;
}

// A still polygon standing before a rounded wall - a sign laid on a pillar, a frame on a curved wall - is swallowed
// where the wall's round comes out past it. The wall is held back there (HoldBehind), place by place, by each still
// polygon of the level near it - one drawn as a wall (no mover's, no portal), not itself moved by the round, and lying
// along the surface there: its plane turned from the way the place faces by no more than HoldAlong's angle, either way
// round. A place moving out of its surface whose way, from where it was to where the round takes it, goes into such a
// polygon that faces it - the back of a thing standing off the wall -, or out through one that faces the way it does -
// the front of a thing laid flat on the wall, whose back is the wall itself and no polygon of the level -, or through a
// sheet drawn from both sides, goes no further than HoldGap short of the plane if the crossing lies within the
// polygon's outline or within HoldRimCaps of the round's cap outside it; further out it gives back less, a smoothstep
// over the spread (HoldSpreadCaps of the cap), and from there on nothing. A polygon standing across the surface - a
// table's leg under its rounded rim - holds nothing: the round goes past its plane into the solid behind it, as it
// always did, and places held there would pile up on its plane. Nor does one whose plane the place starts in
// (HoldOnPlane). A field over the surface, not a mark on a few places: the triangles between the places stay behind
// the polygon too, and the wall comes back to its round without a crease. A place no such polygon is near keeps its
// move to the bit. Places, triangles and polygons as RoundOnce has them at its seventh step; the round's later steps
// are made from the places as held. False where there was no memory.
static const double HoldGap        = 0.5;    // units: how far short of a still polygon's plane a held place stops
static const double HoldRimCaps    = 0.5;    // the rim outside the outline that holds fully, in the round's caps
static const double HoldSpreadCaps = 2.0;    // the spread beyond the rim, in the round's caps
static const double HoldCell       = 128.0;  // units: the cells the still polygons are found by
// The least cosine between a still polygon's normal and the way a place faces, either way round, for the polygon to lie
// along the surface there (about 26 degrees): a sign on a wall lies along it, a table's leg under its rim's foot lies
// across.
static const double HoldAlong      = 0.9;
// Units: a place this near a still polygon's plane lies in it - the engine's own thickness of a plane (its BSP's
// THRESH_POINT_ON_PLANE) - and is not held by it: that is a neighbour in the same plane, a hair off it only by the
// level's floats, which the round may move past.
static const double HoldOnPlane    = 0.1;

namespace Detail
{

// A cell of HoldCell units, packed with an index below it: the cell's three whole-number places, each offset to be
// positive within ten bits, and the index's 24 bits.
inline long long HoldKey(int X, int Y, int Z) { return ((long long)(X + 512) << 20) | ((long long)(Y + 512) << 10) | (Z + 512); }
inline int       HoldCellOf(double V)
{
    const int C = (int)floor(V / HoldCell);
    return (C < -511) ? -511 : ((C > 510) ? 510 : C);
}
// The range of a sorted list of packed cells-and-indices that is cell Key's.
inline void HoldRange(const TList<long long>& Items, long long Key, int& First, int& End)
{
    const long long Lo = Key << 24, Hi = (Key + 1) << 24;
    int A = 0, B = Items.Num;
    while (A < B)
    {
        const int M = (A + B) / 2;
        if (Items[M] < Lo)
            A = M + 1;
        else
            B = M;
    }
    First = A;
    B     = Items.Num;
    while (A < B)
    {
        const int M = (A + B) / 2;
        if (Items[M] < Hi)
            A = M + 1;
        else
            B = M;
    }
    End = A;
}
// How far X, a place on polygon P's plane, lies outside its outline: nought within it, else the distance to its nearest
// side.
inline double HoldOutside(const FV3* Points, const FPolyIn& P, const FV3& X)
{
    // Within: on the same side of every one of its sides (either way round).
    const FV3 N    = RdxSphere::V3(P.Nx, P.Ny, P.Nz);
    int       Pos  = 0, Neg = 0;
    double    Near = 1e300;
    for (int k = 0; k < P.Count; k++)
    {
        const FV3&   Q0 = Points[P.First + k];
        const FV3&   Q1 = Points[P.First + (k + 1) % P.Count];
        const FV3    E  = Q1 - Q0;
        const double L2 = RdxSphere::Dot(E, E);
        if (L2 < 1e-18)
            continue;
        const double S = RdxSphere::Dot(RdxSphere::Cross(E, X - Q0), N);
        if (S > 0.0)
            Pos++;
        else if (S < 0.0)
            Neg++;
        const double T = MaxOf(0.0, MinOf(1.0, RdxSphere::Dot(X - Q0, E) / L2));
        Near           = MinOf(Near, RdxSphere::Length(Q0 + E * T - X));
    }
    return (Pos == 0 || Neg == 0) ? 0.0 : Near;
}

} // namespace Detail

inline bool HoldBehind(const FV3* Points, const FPolyIn* Polys, int NumPolys, const TList<int>& TriPoly,
                       const TList<int>& TriAt, double Cap, FResult& Out)
{
    Out.HeldStopped = Out.HeldEased = 0;
    Out.PlaceHeld.Clear();
    Out.PolyHeld.Clear();
    const int Places = Out.Orig.Num;
    if (Places == 0 || NumPolys == 0 || !(Cap > 0.0))
        return true;
    const double Rim = HoldRimCaps * Cap, Spread = HoldSpreadCaps * Cap, Reach = Rim + Spread;
    // Which polygons the round moves: one with a corner of its triangles that moves, as the seventh step counts them.
    TList<unsigned char> Moving;
    if (!Moving.Resize(NumPolys))
        return false;
    for (int p = 0; p < NumPolys; p++)
        Moving[p] = 0;
    for (int t = 0; t < TriPoly.Num; t++)
        for (int k = 0; k < 3; k++)
        {
            const int v = TriAt[t * 3 + k];
            if (RdxSphere::Length(Out.Moved[v] - Out.Orig[v]) > 1e-4)
                Moving[TriPoly[t]] = 1;
        }
    // The way each place's polygons face, added up over its triangles: only a place that moves that way - out of its
    // surface, towards what stands before it - can come through something; one that goes back into its solid, as a
    // ring's inner wall rounds, or along its surface, leaves what stands before it alone.
    TList<FV3> Facing;
    if (!Facing.Resize(Places))
        return false;
    for (int v = 0; v < Places; v++)
        Facing[v] = RdxSphere::V3(0.0, 0.0, 0.0);
    for (int t = 0; t < TriPoly.Num; t++)
    {
        const FPolyIn& P = Polys[TriPoly[t]];
        for (int k = 0; k < 3; k++)
            Facing[TriAt[t * 3 + k]] = Facing[TriAt[t * 3 + k]] + RdxSphere::V3(P.Nx, P.Ny, P.Nz);
    }
    // The still polygons by the cells their boxes, grown by the reach, touch.
    TList<long long> Still;
    for (int p = 0; p < NumPolys; p++)
    {
        const FPolyIn& P = Polys[p];
        if (!(P.Use & RdxSphere::Use_Wall) || P.Count < 3 || Moving[p])
            continue;
        double Lo[3] = { 1e300, 1e300, 1e300 }, Hi[3] = { -1e300, -1e300, -1e300 };
        for (int k = 0; k < P.Count; k++)
        {
            const FV3& Q = Points[P.First + k];
            Lo[0] = MinOf(Lo[0], Q.X), Hi[0] = MaxOf(Hi[0], Q.X);
            Lo[1] = MinOf(Lo[1], Q.Y), Hi[1] = MaxOf(Hi[1], Q.Y);
            Lo[2] = MinOf(Lo[2], Q.Z), Hi[2] = MaxOf(Hi[2], Q.Z);
        }
        for (int x = Detail::HoldCellOf(Lo[0] - Reach); x <= Detail::HoldCellOf(Hi[0] + Reach); x++)
            for (int y = Detail::HoldCellOf(Lo[1] - Reach); y <= Detail::HoldCellOf(Hi[1] + Reach); y++)
                for (int z = Detail::HoldCellOf(Lo[2] - Reach); z <= Detail::HoldCellOf(Hi[2] + Reach); z++)
                    if (!Still.Add((Detail::HoldKey(x, y, z) << 24) | (long long)p))
                        return false;
    }
    if (Still.Num == 0)
        return true;
    Detail::SortSides(Still.Data, Still.Num);
    // Each moving place: the least share of its move any still polygon near it leaves it. A polygon is met once in a
    // place's cell: its box was put into every cell its grown box touches, so the place's own cell holds it.
    for (int v = 0; v < Places; v++)
    {
        const FV3    A   = Out.Orig[v], B = Out.Moved[v];
        const double Len = RdxSphere::Length(B - A), Faces = RdxSphere::Length(Facing[v]);
        if (!(Len > 1e-4) || !(Faces > 1e-9) || !(RdxSphere::Dot(B - A, Facing[v]) > 0.0))
            continue;
        const FV3 Way     = Facing[v] * (1.0 / Faces);
        double    K       = 1.0;
        bool      bInside = false;
        int       i0, i1;
        Detail::HoldRange(Still, Detail::HoldKey(Detail::HoldCellOf(A.X), Detail::HoldCellOf(A.Y), Detail::HoldCellOf(A.Z)),
                          i0, i1);
        for (int i = i0; i < i1; i++)
        {
            const FPolyIn& P     = Polys[(int)(Still[i] & 0xFFFFFF)];
            const FV3      N     = RdxSphere::V3(P.Nx, P.Ny, P.Nz);
            const double   Along = RdxSphere::Dot(N, Way);
            if (fabs(Along) < HoldAlong)
                continue;
            // Into a polygon that faces the surface, out through one that faces its way - the place goes forward, so
            // only one of the two can be its way through -, either way through a sheet drawn from both sides; not from
            // in its plane.
            const double D0 = RdxSphere::Dot(N, A) - P.W, D1 = RdxSphere::Dot(N, B) - P.W;
            if (fabs(D0) <= HoldOnPlane)
                continue;
            const bool   bThrough = (P.Use & RdxSphere::Use_Sheet) ? ((D0 > 0.0) != (D1 > 0.0))
                                    : (Along < 0.0)                ? (D0 > 0.0 && D1 <= 0.0)
                                                                   : (D0 < 0.0 && D1 >= 0.0);
            if (!bThrough || D0 == D1)
                continue;
            const double T = D0 / (D0 - D1);
            if (!(T > 1e-6))
                continue;
            const double Outside = Detail::HoldOutside(Points, P, A + (B - A) * T);
            if (Outside >= Reach)
                continue;
            const double Limit = MaxOf(0.0, T * Len - HoldGap) / Len;
            const double E     = MaxOf(0.0, Outside - Rim) / Spread;
            const double Ease  = E * E * (3.0 - 2.0 * E);
            K                  = MinOf(K, Limit + (1.0 - Limit) * Ease);
            bInside            = bInside || Outside <= Rim;
        }
        if (K < 1.0)
        {
            if (Out.PlaceHeld.Num == 0)
            {
                if (!Out.PlaceHeld.Resize(Places))
                    return false;
                for (int u = 0; u < Places; u++)
                    Out.PlaceHeld[u] = 0;
            }
            Out.Moved[v]     = A + (B - A) * K;
            Out.PlaceHeld[v] = 1;
            if (bInside)
                Out.HeldStopped++;
            else
                Out.HeldEased++;
        }
    }
    if (Out.PlaceHeld.Num > 0)
    {
        if (!Out.PolyHeld.Resize(NumPolys))
            return false;
        for (int p = 0; p < NumPolys; p++)
            Out.PolyHeld[p] = 0;
        for (int t = 0; t < TriPoly.Num; t++)
            for (int k = 0; k < 3; k++)
                if (Out.PlaceHeld[TriAt[t * 3 + k]])
                    Out.PolyHeld[TriPoly[t]] = 1;
    }
    return true;
}

// The level's polygons rounded into Out: Points and Polys as Spherify.h takes them, Cap the most any point moves, Angle
// the SpherifierAngle (two faces meeting at it or wider are one surface). False where there was no memory for it.
inline bool RoundOnce(const FV3* Points, int NumPoints, const FPolyIn* Polys, int NumPolys, double Cap, double Angle,
                      FResult& Out)
{
    using namespace Detail;
    using RdxMeshRound::FV;
    Out.Orig.Clear();
    Out.Moved.Clear();
    Out.Tri.Clear();
    Out.TriTurn.Clear();
    Out.PolyFaceFirst.Clear();
    Out.PolyFaceCount.Clear();
    Out.FaceKind.Clear();
    Out.FaceCorner.Clear();
    Out.FaceTerm.Clear();
    Out.MoveCap   = Cap;
    Out.PolyGridW = Out.PolyGridH = 0;
    Out.PolyGridFirst.Clear();
    Out.PolyGridCount.Clear();
    Out.PolyGridItems.Clear();
    Out.Left.Clear();
    Out.PolyEdgeKinds.Clear();
    Out.PolySideFirst.Clear();
    Out.PolySideCount.Clear();
    Out.LightSides.Clear();
    Out.LightParts.Clear();
    Out.Walls = Out.Segments = Out.SoftEdges = Out.Creases = Out.TPoints = Out.Folded = Out.Touched = 0;
    Out.Through = Out.Stuck = Out.Pieces = Out.Quads = Out.Unbent = Out.Zipped = 0;
    Out.LargestMove = 0.0;
    for (int k = 0; k < FResult::Parts; k++)
        Out.PartMs[k] = 0.0;
    if (!Out.PolyFirst.Resize(NumPolys) || !Out.PolyTris.Resize(NumPolys) || !Out.PolyRelief.Resize(NumPolys) ||
        !Out.PolyPlane.Resize(4 * NumPolys))
        return false;
    for (int p = 0; p < NumPolys; p++)
    {
        Out.PolyFirst[p]         = 0;
        Out.PolyTris[p]          = 0;
        Out.PolyRelief[p]        = (Polys[p].Use & RdxSphere::Use_Relief) ? 1 : 0;
        Out.PolyPlane[4 * p]     = (float)Polys[p].Nx;
        Out.PolyPlane[4 * p + 1] = (float)Polys[p].Ny;
        Out.PolyPlane[4 * p + 2] = (float)Polys[p].Nz;
        Out.PolyPlane[4 * p + 3] = (float)Polys[p].W;
    }
    double T0 = Out.Seconds ? Out.Seconds() : 0.0;
    auto   Lap = [&](int Part)
    {
        if (!Out.Seconds)
            return;
        const double T1 = Out.Seconds();
        Out.PartMs[Part] += (T1 - T0) * 1000.0;
        T0 = T1;
    };

    // 1. The places the drawn polygons use, welded within WeldDist.
    TList<int> WeldOf;    // an input point: its welded place, or -1
    TList<FV3> Place;     // a welded place
    if (!WeldOf.Resize(NumPoints))
        return false;
    for (int i = 0; i < NumPoints; i++)
        WeldOf[i] = -1;
    FCells Welds;
    if (!Welds.Init(NumPoints))
        return false;
    const double Inv = 1.0 / WeldDist;
    for (int p = 0; p < NumPolys; p++)
    {
        const FPolyIn& P = Polys[p];
        if (!(P.Use & RdxSphere::Use_Wall) || P.Count < 3)
            continue;
        for (int k = 0; k < P.Count; k++)
        {
            const int i = P.First + k;
            if (i < 0 || i >= NumPoints || WeldOf[i] >= 0)
                continue;
            const FV3&      Q  = Points[i];
            const long long CX = (long long)floor(Q.X * Inv), CY = (long long)floor(Q.Y * Inv),
                            CZ = (long long)floor(Q.Z * Inv);
            int Found = -1;
            for (int dz = -1; dz <= 1 && Found < 0; dz++)
                for (int dy = -1; dy <= 1 && Found < 0; dy++)
                    for (int dx = -1; dx <= 1 && Found < 0; dx++)
                    {
                        const long long K = CellKey(CX + dx, CY + dy, CZ + dz);
                        for (int e = Welds.Head[Welds.Bucket(K)]; e >= 0; e = Welds.Next[e])
                            if (Welds.Key[e] == K && Dist2(Place[Welds.Item[e]], Q) <= WeldDist * WeldDist)
                            {
                                Found = Welds.Item[e];
                                break;
                            }
                    }
            if (Found < 0)
            {
                Found = Place.Num;
                if (!Place.Add(Q) || !Welds.Add(CellKey(CX, CY, CZ), Found))
                    return false;
            }
            WeldOf[i] = Found;
        }
    }
    Lap(0);

    // 2. Each drawn polygon's ring of places, its T-junctions put in: a place of the level's lying on one of its edges.
    TList<int>           RingFirst, RingCount, Ring;
    TList<unsigned char> RingOwn;   // a place of a ring: 1 where it is one of the polygon's own corners
    if (!RingFirst.Resize(NumPolys) || !RingCount.Resize(NumPolys))
        return false;
    FCells Near;
    if (!Near.Init(Place.Num))
        return false;
    const double InvCell = 1.0 / GridCell;
    for (int w = 0; w < Place.Num; w++)
    {
        const FV3& Q = Place[w];
        if (!Near.Add(CellKey((long long)floor(Q.X * InvCell), (long long)floor(Q.Y * InvCell),
                              (long long)floor(Q.Z * InvCell)), w))
            return false;
    }
    TList<int>    Base;
    TList<int>    Found;
    TList<double> FoundAt;
    for (int p = 0; p < NumPolys; p++)
    {
        RingFirst[p] = Ring.Num;
        RingCount[p] = 0;
        const FPolyIn& P = Polys[p];
        if (!(P.Use & RdxSphere::Use_Wall) || P.Count < 3)
            continue;
        Base.Clear();
        for (int k = 0; k < P.Count; k++)
        {
            const int i = P.First + k;
            const int w = (i >= 0 && i < NumPoints) ? WeldOf[i] : -1;
            if (w >= 0 && (Base.Num == 0 || Base[Base.Num - 1] != w))
                if (!Base.Add(w))
                    return false;
        }
        while (Base.Num > 1 && Base[0] == Base[Base.Num - 1])
            Base.Num--;
        if (Base.Num < 3)
            continue;
        for (int k = 0; k < Base.Num; k++)
        {
            const int A = Base[k], B = Base[(k + 1) % Base.Num];
            if (!Ring.Add(A) || !RingOwn.Add(1))
                return false;
            const FV3& PA = Place[A];
            const FV3& PB = Place[B];
            const FV3  E  = PB - PA;
            const double L2 = RdxSphere::Dot(E, E);
            if (L2 < 1e-12)
                continue;
            Found.Clear();
            FoundAt.Clear();
            const long long X0 = (long long)floor((MinOf(PA.X, PB.X) - OnEdge) * InvCell),
                            X1 = (long long)floor((MaxOf(PA.X, PB.X) + OnEdge) * InvCell);
            const long long Y0 = (long long)floor((MinOf(PA.Y, PB.Y) - OnEdge) * InvCell),
                            Y1 = (long long)floor((MaxOf(PA.Y, PB.Y) + OnEdge) * InvCell);
            const long long Z0 = (long long)floor((MinOf(PA.Z, PB.Z) - OnEdge) * InvCell),
                            Z1 = (long long)floor((MaxOf(PA.Z, PB.Z) + OnEdge) * InvCell);
            if ((X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1) > 4096)
                continue;   // an edge across the whole level: its T-junctions are not looked for
            for (long long cz = Z0; cz <= Z1; cz++)
                for (long long cy = Y0; cy <= Y1; cy++)
                    for (long long cx = X0; cx <= X1; cx++)
                    {
                        const long long Kc = CellKey(cx, cy, cz);
                        for (int e = Near.Head[Near.Bucket(Kc)]; e >= 0; e = Near.Next[e])
                        {
                            if (Near.Key[e] != Kc)
                                continue;
                            const int w = Near.Item[e];
                            if (w == A || w == B)
                                continue;
                            const double T = RdxSphere::Dot(Place[w] - PA, E) / L2;
                            if (!(T > 1e-6 && T < 1.0 - 1e-6))
                                continue;
                            if (Dist2(PA + E * T, Place[w]) > OnEdge * OnEdge)
                                continue;
                            if (!Found.Add(w) || !FoundAt.Add(T))
                                return false;
                        }
                    }
            // Along the edge, in order (few: a sort by insertion).
            for (int a = 1; a < Found.Num; a++)
                for (int b = a; b > 0 && FoundAt[b] < FoundAt[b - 1]; b--)
                {
                    const double Tt = FoundAt[b];
                    FoundAt[b]      = FoundAt[b - 1];
                    FoundAt[b - 1]  = Tt;
                    const int Tw    = Found[b];
                    Found[b]        = Found[b - 1];
                    Found[b - 1]    = Tw;
                }
            for (int a = 0; a < Found.Num; a++)
                if (Ring[Ring.Num - 1] != Found[a])
                {
                    if (!Ring.Add(Found[a]) || !RingOwn.Add(0))
                        return false;
                    Out.TPoints++;
                }
        }
        RingCount[p] = Ring.Num - RingFirst[p];
        Out.Walls++;
    }
    Lap(1);

    // 3. Each ring cut into triangles over its own corners: the shortest cuts (a polygon's triangulation by its least
    //    total diagonal), no triangle of no area or lying along an edge. A ring with more corners than MostRing, or
    //    that cannot be so cut, is left to the engine.
    TList<int>    Corner;    // 3 a triangle: places
    TList<int>    FaceOf;    // a triangle: its polygon
    TList<int>    PolyFace0, PolyFaces;   // a polygon: its first triangle and how many - made one after another
    TList<double> Cost;
    TList<int>    Split;
    TList<int>    Stack;
    if (!PolyFace0.Resize(NumPolys) || !PolyFaces.Resize(NumPolys))
        return false;
    for (int p = 0; p < NumPolys; p++)
        PolyFace0[p] = PolyFaces[p] = 0;
    // The sides of the sheets' rings (Use_Sheet), two places each, sorted: a wall a sheet lies across is cut along the
    // sheet's side where its ring has both ends, so that the side is the two walls' and the sheet's (FShape::SheetAlong)
    // and the sheet goes along. Cut otherwise, the side would be the sheet's alone, and it would stay where it is.
    TList<long long> SheetSides;
    for (int p = 0; p < NumPolys; p++)
    {
        const int n = RingCount[p];
        if (!(Polys[p].Use & RdxSphere::Use_Sheet) || n < 3)
            continue;
        for (int k = 0; k < n; k++)
        {
            const long long A = Ring[RingFirst[p] + k], B = Ring[RingFirst[p] + (k + 1) % n];
            if (!SheetSides.Add((A < B) ? ((A << 32) | B) : ((B << 32) | A)))
                return false;
        }
    }
    Detail::SortSides(SheetSides.Data, SheetSides.Num);
    auto IsSheetSide = [&](long long A, long long B)
    {
        const long long K  = (A < B) ? ((A << 32) | B) : ((B << 32) | A);
        int             Lo = 0, Hi = SheetSides.Num - 1;
        while (Lo <= Hi)
        {
            const int Mid = (Lo + Hi) / 2;
            if (SheetSides[Mid] == K)
                return true;
            if (SheetSides[Mid] < K)
                Lo = Mid + 1;
            else
                Hi = Mid - 1;
        }
        return false;
    };
    for (int p = 0; p < NumPolys; p++)
    {
        PolyFace0[p] = FaceOf.Num;
        const int n  = RingCount[p];
        if (n < 3)
            continue;
        if (n > MostRing)
        {
            Out.Folded++;
            if (!Out.Left.Add(p))
                return false;
            continue;
        }
        const int* const R = &Ring[RingFirst[p]];
        if (!Cost.Resize(n * n) || !Split.Resize(n * n))
            return false;
        // A triangle of no area - its longest side's square times a millionth or less - and, where the ring can be cut
        // without, one with a corner on the middle of its longest side, within Straight of it: a T-junction's place with
        // the two ends of the edge it lies on, or a corner of the polygon's own on the line of two others. The side is
        // pinned at the corner on it and the cut across is not, so the two would bow apart over a sliver too thin to hold
        // it, and fold it over; the corner is cut to one further off instead. A ring that cannot be cut so - a polygon
        // thin all along - is cut as it comes.
        bool bStrict = true;
        auto Flat    = [&](int a, int b, int c)
        {
            const FV3    U = Place[R[b]] - Place[R[a]], W = Place[R[c]] - Place[R[a]];
            const FV3    X = RdxSphere::Cross(U, W);
            const double Longest = MaxOf(RdxSphere::Dot(U, U), MaxOf(RdxSphere::Dot(W, W),
                                        RdxSphere::Dot(W - U, W - U)));
            if (RdxSphere::Dot(X, X) <= 1e-12 * Longest * Longest)
                return true;
            if (!bStrict)
                return false;
            // The longest side and the corner across it.
            const int S[3][3] = { { a, b, c }, { b, c, a }, { c, a, b } };
            for (int k = 0; k < 3; k++)
            {
                const FV3    P = Place[R[S[k][0]]], Q = Place[R[S[k][1]]], C = Place[R[S[k][2]]];
                const FV3    Side = Q - P;
                const double L2   = RdxSphere::Dot(Side, Side);
                if (L2 < Longest)
                    continue;
                const double L = sqrt(L2), Along = RdxSphere::Dot(C - P, Side) / L;
                const double Off = RdxSphere::Length(RdxSphere::Cross(C - P, Side)) / L;
                return Off < Straight && Along > Straight && Along < L - Straight;
            }
            return false;
        };
        const double Inf = 1e300;
        for (int Pass = 0; Pass < 2 && (Pass == 0 || Cost[n - 1] >= Inf); Pass++)
        {
            bStrict = Pass == 0;
            for (int i = 0; i < n; i++)
                for (int j = 0; j < n; j++)
                {
                    Cost[i * n + j]  = (j == i + 1) ? 0.0 : Inf;
                    Split[i * n + j] = -1;
                }
            const bool bWall = !(Polys[p].Use & RdxSphere::Use_Sheet) && SheetSides.Num > 0;
            for (int Gap = 2; Gap < n; Gap++)
                for (int i = 0; i + Gap < n; i++)
                {
                    const int    j    = i + Gap;
                    const double Side = (i == 0 && j == n - 1)                ? 0.0
                                        : (bWall && IsSheetSide(R[i], R[j])) ? -SheetCutGain
                                                                              : RdxSphere::Length(Place[R[j]] - Place[R[i]]);
                    double       Best = Inf;
                    int          At   = -1;
                    for (int k = i + 1; k < j; k++)
                    {
                        if (Cost[i * n + k] >= Inf || Cost[k * n + j] >= Inf || Flat(i, k, j))
                            continue;
                        const double C = Cost[i * n + k] + Cost[k * n + j] + Side;
                        if (C < Best)
                        {
                            Best = C;
                            At   = k;
                        }
                    }
                    Cost[i * n + j]  = Best;
                    Split[i * n + j] = At;
                }
        }
        if (Cost[n - 1] >= Inf)
        {
            Out.Folded++;
            if (!Out.Left.Add(p))
                return false;
            continue;
        }
        Stack.Clear();
        if (!Stack.Add(0) || !Stack.Add(n - 1))
            return false;
        while (Stack.Num >= 2)
        {
            const int j = Stack[--Stack.Num];
            const int i = Stack[--Stack.Num];
            if (j - i < 2)
                continue;
            const int k = Split[i * n + j];
            if (k < 0)
                continue;
            if (!Corner.Add(R[i]) || !Corner.Add(R[k]) || !Corner.Add(R[j]) || !FaceOf.Add(p) || !Stack.Add(i) ||
                !Stack.Add(k) || !Stack.Add(k) || !Stack.Add(j))
                return false;
        }
        PolyFaces[p] = FaceOf.Num - PolyFace0[p];
    }
    const int Faces = FaceOf.Num;
    Out.Segments    = Faces;
    if (Faces == 0)
        return true;

    Lap(2);

    // 4. The level's triangles as one model, and its shape by the models' rule.
    TList<FV> At;
    if (!At.Resize(Place.Num))
        return false;
    for (int w = 0; w < Place.Num; w++)
        At[w] = RdxMeshRound::V((float)Place[w].X, (float)Place[w].Y, (float)Place[w].Z);
    RdxMeshRound::FModel Model;
    if (!Model.Make(Corner.Data, Faces, Place.Num))
        return false;
    RdxMeshRound::FPose Pose;
    Pose.Base   = (const char*)At.Data;
    Pose.Stride = (int)sizeof(FV);
    RdxMeshRound::FShape* const Shape = new RdxMeshRound::FShape;
    if (!Shape)
        return false;
    RdxMeshRound::FShape& S = *Shape;
    // The faces of the sheets drawn from both sides (Spherify.h's Use_Sheet).
    TList<unsigned char> Sheet;
    if (!Sheet.Resize(Faces))
    {
        delete Shape;
        return false;
    }
    for (int f = 0; f < Faces; f++)
        Sheet[f] = (Polys[FaceOf[f]].Use & RdxSphere::Use_Sheet) ? 1 : 0;

    // 5. The flat polygons as one piece each (FCoarse), and the shape made of the pieces by the models' rule: the soft
    //    edges and creases, each edge's terms - the rounded surface (FSurface), which the cut polygons are laid on, so
    //    that nothing of the BSP's cutting pins it. A sheet that goes along with what it lies across has no piece: it
    //    moves with that.
    //    Made once more, with the shape of step 4 before it, where the surface would move a sheet going along off its
    //    plane on any of its sides: then it would turn over where it shows - it holds those sides instead, as a sheet
    //    across nothing does.
    TList<unsigned char> AlongPoly, Skip;
    FCoarse              Coarse;
    // The surface made last, in its own block - made again where a sheet is held - and given back on the way out.
    struct FSurfaceHold
    {
        FSurface* Made = nullptr;
        ~FSurfaceHold() { delete Made; }
    } SurfHold;
    if (!AlongPoly.Resize(Faces) || !Skip.Resize(Faces))
    {
        delete Shape;
        return false;
    }
    for (int Pass = 0; Pass < 2; Pass++)
    {
        if (!S.Make(Model, nullptr, Pose, (float)(180.0 - Angle), (float)Cap, (float)(WeldDist * 0.1), false, 0.0f,
                    Sheet.Data, true))
        {
            delete Shape;
            return false;
        }
        S.Pose(Pose, 1);
        if (Pass == 0)
            Lap(3);
        for (int f = 0; f < Faces; f++)
            AlongPoly[f] = 0;
        for (int t = 0; t + 2 < S.SheetAlong.Num; t += 3)
            AlongPoly[S.Poly[S.SheetAlong[t]]] = 1;
        for (int f = 0; f < Faces; f++)
            Skip[f] = AlongPoly[S.Poly[f]];
        if (!Coarsen(S, Place, Corner, Faces, Coarse, Skip.Data))
        {
            delete Shape;
            return false;
        }
        Out.Through   = Coarse.Through;
        Out.Stuck     = Coarse.Stuck;
        Out.Pieces    = Coarse.Region.Num;
        Out.SoftEdges = Out.Creases = 0;
        delete SurfHold.Made;
        SurfHold.Made = new FSurface;
        if (!SurfHold.Made)
        {
            delete Shape;
            return false;
        }
        FSurface&                   Made = *SurfHold.Made;
        RdxMeshRound::FModel        PieceModel;
        RdxMeshRound::FShape* const PieceShape = new RdxMeshRound::FShape;
        // A piece is a sheet's where its flat polygon is.
        TList<unsigned char> PieceSheet;
        bool bMade = PieceSheet.Resize(Coarse.Region.Num);
        for (int i = 0; bMade && i < Coarse.Region.Num; i++)
            PieceSheet[i] = Sheet[Coarse.Region[i]];
        bMade = bMade && PieceShape && Coarse.Region.Num > 0 &&
                PieceModel.Make(Coarse.Tri.Data, Coarse.Region.Num, Place.Num) &&
                PieceShape->Make(PieceModel, nullptr, Pose, (float)(180.0 - Angle), (float)Cap,
                                 (float)(WeldDist * 0.1), false, 0.0f, PieceSheet.Data, true);
        if (bMade)
        {
            PieceShape->Pose(Pose, 1);
            for (int E = 0; E < PieceShape->Edges; E++)
            {
                Out.SoftEdges += (PieceShape->EdgeKind[E] == RdxMeshRound::FShape::Edge_Soft) ? 1 : 0;
                Out.Creases += (PieceShape->EdgeKind[E] == RdxMeshRound::FShape::Edge_Crease) ? 1 : 0;
            }
            bMade = SurfaceOf(*PieceShape, Coarse, Place, Faces, (float)Cap, Made, Out.Workers) &&
                    QuadsOf(*PieceShape, Coarse, Place, Made) &&
                    UnfoldSurface(Made, PieceShape->Edges, Out.Unbent, Out.Workers);
            Out.Quads = Made.QuadDrop.Num;
        }
        delete PieceShape;
        if (!bMade)
        {
            delete Shape;
            return false;
        }
        if (Pass > 0)
            break;
        // Each side a sheet goes along on (the edge of its face that two faces in one plane share), the surface there at
        // its ends and at its quarters, off the sheet's plane: past TurnInPlane, the sheet's flat polygon is held.
        bool bHeld = false;
        for (int t = 0; t + 2 < S.SheetAlong.Num; t += 3)
        {
            const int Fk = S.SheetAlong[t], Fi = S.SheetAlong[t + 1];
            if (!Sheet[Fk])
                continue;
            const FPolyIn& Ps = Polys[FaceOf[Fk]];
            const FV3      Ns = RdxSphere::V3(Ps.Nx, Ps.Ny, Ps.Nz);
            const int      R  = S.Poly[Fi];
            bool           bOff = false;
            for (int k = 0; k < 3 && !bOff; k++)
            {
                const int E = S.EdgeOf[Fk * 3 + k];
                if (S.EdgeCount[E] != 3 || !(S.EdgeC0[E] / 3 == Fi || S.EdgeC1[E] / 3 == Fi))
                    continue;
                const FV3 A = Place[S.EdgeA[E]], B = Place[S.EdgeB[E]];
                for (int q = 0; q <= 4 && !bOff; q++)
                {
                    const int w = (q == 0) ? S.EdgeA[E] : (q == 4) ? S.EdgeB[E] : -1;
                    if (w >= 0 && !(Coarse.Taken[w] && !Coarse.Still[w]))
                        continue;
                    const FV M = Made.MoveAt(R, A + (B - A) * (q * 0.25));
                    bOff       = fabs(M.X * Ns.X + M.Y * Ns.Y + M.Z * Ns.Z) > TurnInPlane;
                }
            }
            if (!bOff)
                continue;
            const int Flat = S.Poly[Fk];
            for (int f = 0; f < Faces; f++)
                if (S.Poly[f] == Flat)
                    Sheet[f] = 0;
            bHeld = true;
#ifdef LEVELROUND_TRACE
            LevelRoundTraceSheetHeld(FaceOf[Fk]);
#endif
        }
        Lap(4);
        if (!bHeld)
            break;
    }
    FSurface& Surf = *SurfHold.Made;
    Lap(5);

    // 6. Every point moved with the surface: the level's own places - a corner of the shape stays where it is -, each
    //    edge's pieces, as many as its curve needs to stand off them by no more than Sag and made once for both of its
    //    sides, and each triangle's inside cut into a grid as fine as its edges' middles or its own middle need, its
    //    points on an edge cut fewer times put on that edge's own pieces, those of an edge cut more joined by fans.
    TList<int> RegionOf;   // a place: the flat polygon one of its triangles lies in
    TList<FV>  PlaceMove;
    if (!RegionOf.Resize(Place.Num) || !PlaceMove.Resize(Place.Num))
    {
        delete Shape;
        return false;
    }
    for (int w = 0; w < Place.Num; w++)
        RegionOf[w] = -1;
    // A sheet's that goes along first, so that a place it shares with what it goes along with reads that one's surface.
    for (int Pass = 0; Pass < 2; Pass++)
        for (int f = 0; f < Faces; f++)
            if ((Skip[f] != 0) == (Pass == 0))
                for (int k = 0; k < 3; k++)
                    RegionOf[S.Weld[Corner[f * 3 + k]]] = S.Poly[f];
    for (int w = 0; w < Place.Num; w++)
    {
        PlaceMove[w] = (Coarse.Taken[w] && !Coarse.Still[w]) ? Surf.MoveAt(RegionOf[w], Place[w])
                                                             : RdxMeshRound::V(0.0f, 0.0f, 0.0f);
        if (!Out.Orig.Add(Place[w]) ||
            !Out.Moved.Add(Place[w] + RdxSphere::V3(PlaceMove[w].X, PlaceMove[w].Y, PlaceMove[w].Z)))
        {
            delete Shape;
            return false;
        }
    }
    // The edges cut and the faces laid on the surface; then, up to TurnPasses times in all, where a face turns over where
    // it shows (ShowsTurned), each edge of it cut finer for a bend done near its corner goes back to the pieces its
    // middles ask for, and the faces on it are laid again.
    TList<int>           Grid;
    TList<int>           TriPoly;
    TList<int>           TriAt;
    TList<unsigned char> Revert, Finer;   // an edge: taken back to its middles' pieces; cut finer than they ask
    TList<unsigned char> Changed;         // an edge: taken back on this pass
    TList<int>           Turned;          // the faces the pass turned over where they show
    TList<int>           FaceTri0, FaceTriN;   // a face: where its triangles start in TriPoly, and how many
    TList<unsigned char> FaceZipped;      // a face: laid by its sides' points (Zip)
    if (!Revert.Resize(S.Edges) || !Finer.Resize(S.Edges) || !Changed.Resize(S.Edges) || !FaceTri0.Resize(Faces) ||
        !FaceTriN.Resize(Faces) || !FaceZipped.Resize(Faces))
    {
        delete Shape;
        return false;
    }
    for (int E = 0; E < S.Edges; E++)
        Revert[E] = Finer[E] = Changed[E] = 0;
    // Whether face f's triangles from the T0-th on turn over where it shows: not within TurnInPlane of its polygon's
    // plane - drawn there as what it lies over, its pictures read where it is (the renderer's SphereReadAt) - nor a
    // sliver of less than TurnSliver square units either way.
    auto ShowsTurned = [&](int f, int T0) -> bool
    {
        const FPolyIn& Pl = Polys[FaceOf[f]];
        const FV3      Np = RdxSphere::V3(Pl.Nx, Pl.Ny, Pl.Nz);
        for (int t = T0; t < TriPoly.Num; t++)
        {
            const int A = TriAt[t * 3], B = TriAt[t * 3 + 1], D = TriAt[t * 3 + 2];
            const FV3 Now = RdxSphere::Cross(Out.Moved[B] - Out.Moved[A], Out.Moved[D] - Out.Moved[A]);
            const FV3 Was = RdxSphere::Cross(Out.Orig[B] - Out.Orig[A], Out.Orig[D] - Out.Orig[A]);
            if (RdxSphere::Dot(Now, Was) >= 0.0)
                continue;
            if (fabs(RdxSphere::Dot(Out.Moved[A] - Out.Orig[A], Np)) <= TurnInPlane &&
                fabs(RdxSphere::Dot(Out.Moved[B] - Out.Orig[B], Np)) <= TurnInPlane &&
                fabs(RdxSphere::Dot(Out.Moved[D] - Out.Orig[D], Np)) <= TurnInPlane)
                continue;
            if (0.5 * RdxSphere::Length(Was) < TurnSliver && 0.5 * RdxSphere::Length(Now) < TurnSliver)
                continue;
            return true;
        }
        return false;
    };
    {
        TList<int> Cuts, EdgePoint;   // an edge: its pieces, and where its inner points start in the output
        TList<int> GridCuts;          // an edge: the pieces its middles ask for, which its faces' grids go by
        TList<int> EdgeRegion;        // an edge: the flat polygon its curve is read on
        if (!Cuts.Resize(S.Edges) || !EdgePoint.Resize(S.Edges) || !GridCuts.Resize(S.Edges) ||
            !EdgeRegion.Resize(S.Edges))
        {
            delete Shape;
            return false;
        }
        // Whether an edge between two flat polygons has a wall the relief may be drawn on beside it (Spherify.h's
        // Use_Relief): laid by the card on the rounded surface itself, with the pieces cut here under it and beside it.
        // An edge inside a flat polygon bends it out of its plane alone.
        auto ReliefBeside = [&](int E)
        {
            if (S.EdgeKind[E] == RdxMeshRound::FShape::Edge_Inner)
                return false;
            return (Polys[FaceOf[S.EdgeC0[E] / 3]].Use & RdxSphere::Use_Relief) != 0 ||
                   (S.EdgeC1[E] >= 0 && (Polys[FaceOf[S.EdgeC1[E] / 3]].Use & RdxSphere::Use_Relief) != 0);
        };
        for (int E = 0; E < S.Edges; E++)
        {
            const int a = S.EdgeA[E], b = S.EdgeB[E];
            int       R = S.Poly[S.EdgeC0[E] / 3];
            if (!Surf.Bends[R] && S.EdgeC1[E] >= 0)
                R = S.Poly[S.EdgeC1[E] / 3];
            // Its curve at up to 33 places along it, each worked out when first needed, and the fewest pieces - halved
            // until they are - that stand off it by no more than Sag at their middles; between two flat polygons, one of
            // them a wall the relief may be drawn on, at every one of those places along them. For an edge inside a flat
            // polygon only what stands off its plane counts - a move along it leaves the flat polygon as flat.
            FV   Along[33];
            bool bHave[33] = {};
            int  N      = 1;
            int  Asked  = 1;
            const bool bInner = S.EdgeKind[E] == RdxMeshRound::FShape::Edge_Inner;
            const FV3  Nf     = FaceNormal(Place, Corner, S.EdgeC0[E] / 3);
            auto Off = [&](const FV& D)
            { return bInner ? fabsf((float)(D.X * Nf.X + D.Y * Nf.Y + D.Z * Nf.Z)) : RdxMeshRound::Length(D); };
            auto SampleAt = [&](int k) -> const FV&
            {
                if (!bHave[k])
                {
                    Along[k] = (k == 0) ? PlaceMove[a] : (k == 32) ? PlaceMove[b]
                                                      : Surf.MoveAt(R, Place[a] + (Place[b] - Place[a]) * ((double)k / 32.0));
                    bHave[k] = true;
                }
                return Along[k];
            };
            if (Surf.Bends[R] && S.EdgeKind[E] != RdxMeshRound::FShape::Edge_Fixed)
            {
                // The fewest pieces: by every place along each, or by its middle alone.
                auto Pieces = [&](bool bEvery)
                {
                    int Count = 1;
                    for (; Count < MostCuts; Count *= 2)
                    {
                        const int Step   = 32 / Count;
                        bool      bClose = true;
                        for (int j = 0; j < Count && bClose; j++)
                        {
                            if (!bEvery)
                            {
                                const FV Mid = (SampleAt(j * Step) + SampleAt((j + 1) * Step)) * 0.5f;
                                bClose       = Off(SampleAt(j * Step + Step / 2) - Mid) <= Sag;
                                continue;
                            }
                            const FV From = SampleAt(j * Step), To = SampleAt((j + 1) * Step);
                            for (int k = 1; k < Step && bClose; k++)
                                bClose = Off(SampleAt(j * Step + k) - (From + (To - From) * ((float)k / (float)Step))) <= Sag;
                        }
                        if (bClose)
                            break;
                    }
                    return Count;
                };
                Asked           = Pieces(false);
                const int Every = ReliefBeside(E) ? Pieces(true) : Asked;
                N               = (Every > Asked) ? Every : Asked;
                Finer[E]        = (N > Asked) ? 1 : 0;
            }
            Cuts[E]       = N;
            GridCuts[E]   = Asked;
            EdgeRegion[E] = R;
            EdgePoint[E]  = Out.Orig.Num;
            for (int k = 1; k < N; k++)
            {
                const FV3 Flat = Place[a] + (Place[b] - Place[a]) * ((double)k / (double)N);
                const FV  Move = SampleAt(k * 32 / N);
                if (!Out.Orig.Add(Flat) || !Out.Moved.Add(Flat + RdxSphere::V3(Move.X, Move.Y, Move.Z)))
                {
                    delete Shape;
                    return false;
                }
            }
        }
        // Each face's own need in its middle - where the surface stands off the flat triangle through its corners' moves, off
        // its plane, by more than Sag, a grid as fine as a bow of that height needs - and an edge cut at least half as finely
        // as each face it bounds is (in powers of two, as its curve's samples are): a face's grid then takes at most two of
        // its points onto one of an edge's. A whole row of a fine grid taken onto the two ends of an edge cut once would be a
        // fan of slivers, which the surface's least twist turns over. An open edge stays as it is: nothing is beyond it.
        TList<int> MiddleNeed;
        if (!MiddleNeed.Resize(Faces))
        {
            delete Shape;
            return false;
        }
        for (int f = 0; f < Faces; f++)
        {
            MiddleNeed[f]   = 1;
            const int R = S.Poly[f];
            if (!Surf.Bends[R])
                continue;
            const int   C0 = S.Weld[Corner[f * 3]], C1 = S.Weld[Corner[f * 3 + 1]], C2 = S.Weld[Corner[f * 3 + 2]];
            const FV    Lin = (PlaceMove[C0] + PlaceMove[C1] + PlaceMove[C2]) * (1.0f / 3.0f);
            const FV    D   = Surf.MoveAt(R, (Place[C0] + Place[C1] + Place[C2]) * (1.0 / 3.0)) - Lin;
            const FV3   Nf  = FaceNormal(Place, Corner, f);
            const float Off = fabsf((float)(D.X * Nf.X + D.Y * Nf.Y + D.Z * Nf.Z));
            if (Off > Sag)
            {
                const int Need = (int)ceilf(sqrtf(Off / Sag));
                MiddleNeed[f]      = (Need < MostCuts) ? Need : MostCuts;
            }
        }
        TList<int> Wanted;
        if (!Wanted.Resize(S.Edges))
        {
            delete Shape;
            return false;
        }
        for (int E = 0; E < S.Edges; E++)
            Wanted[E] = GridCuts[E];
        for (bool bAgain = true; bAgain;)
        {
            bAgain = false;
            for (int f = 0; f < Faces; f++)
            {
                if (!Surf.Bends[S.Poly[f]])
                    continue;
                int FaceN = MiddleNeed[f];
                for (int e = 0; e < 3; e++)
                    FaceN = (Wanted[S.EdgeOf[f * 3 + e]] > FaceN) ? Wanted[S.EdgeOf[f * 3 + e]] : FaceN;
                int Half = 1;
                while (Half * 4 < FaceN && Half < MostCuts)
                    Half *= 2;
                for (int e = 0; e < 3; e++)
                {
                    const int E = S.EdgeOf[f * 3 + e];
                    if (Wanted[E] < Half && S.EdgeKind[E] != RdxMeshRound::FShape::Edge_Fixed)
                    {
                        Wanted[E] = Half;
                        bAgain    = true;
                    }
                }
            }
        }
        // An edge cut more finely than its curve asked: its points made again, as many as that. The faces' grids go by
        // what is wanted; an edge cut close to its curve all along keeps its points where they are more.
        for (int E = 0; E < S.Edges; E++)
        {
            GridCuts[E] = Wanted[E];
            if (Wanted[E] <= Cuts[E])
                continue;
            const int a = S.EdgeA[E], b = S.EdgeB[E], N = Wanted[E];
            Cuts[E]      = N;
            EdgePoint[E] = Out.Orig.Num;
            for (int k = 1; k < N; k++)
            {
                const FV3 Flat = Place[a] + (Place[b] - Place[a]) * ((double)k / (double)N);
                const FV  Move = Surf.MoveAt(EdgeRegion[E], Flat);
                if (!Out.Orig.Add(Flat) || !Out.Moved.Add(Flat + RdxSphere::V3(Move.X, Move.Y, Move.Z)))
                {
                    delete Shape;
                    return false;
                }
            }
        }
        // The area the triangles from the T0-th on turned over: each against where it lay flat.
        auto TurnedSince = [&](int T0) -> double
        {
            double Area = 0.0;
            for (int t = T0; t < TriPoly.Num; t++)
            {
                const int A = TriAt[t * 3], B = TriAt[t * 3 + 1], D = TriAt[t * 3 + 2];
                const FV3 Now = RdxSphere::Cross(Out.Moved[B] - Out.Moved[A], Out.Moved[D] - Out.Moved[A]);
                const FV3 Was = RdxSphere::Cross(Out.Orig[B] - Out.Orig[A], Out.Orig[D] - Out.Orig[A]);
                if (RdxSphere::Dot(Now, Was) < 0.0)
                    Area += 0.5 * RdxSphere::Length(Now);
            }
            return Area;
        };
        // A face's points on its sides - none inside it - zipped into triangles across it, into ZipTris (three places each,
        // the face's way round): two rows of its sides' points, each step to whichever row's next point comes first. The
        // grid's triangles are as thin as their face; these reach across it. False: they are no whole cover of the face - a
        // triangle with no area or turned against the face, the area not the face's - or one of them stands off the surface
        // by more than Sag in its middle.
        TList<int> ZipRow[2], ZipTris, FanRing, SidePts[3];
        auto Zip = [&](int R, const int* C, const int* EdgeOfFace, const int* EdgeN, const bool* bForward) -> bool
        {
            // The points of the face's side e, from its corner C[e] to C[e + 1] or back, after those in Into.
            auto Side = [&](int e, bool bBack, TList<int>& Into) -> bool
            {
                const int Ed = EdgeOfFace[e], Ne = EdgeN[e];
                for (int s = 0; s <= Ne; s++)
                {
                    const int Step = bBack ? Ne - s : s;
                    const int k    = bForward[e] ? Step : Ne - Step;
                    const int I    = (k == 0) ? S.EdgeA[Ed] : (k == Ne) ? S.EdgeB[Ed] : EdgePoint[Ed] + k - 1;
                    if (Into.Num > 0 && Into[Into.Num - 1] == I)
                        continue;
                    if (!Into.Add(I))
                        return false;
                }
                return true;
            };
            int    L    = 0;
            double Most = -1.0;
            for (int e = 0; e < 3; e++)
            {
                const double Len = RdxSphere::Length(Place[C[(e + 1) % 3]] - Place[C[e]]);
                if (Len > Most)
                {
                    Most = Len;
                    L    = e;
                }
            }
            ZipRow[0].Clear();
            ZipRow[1].Clear();
            ZipTris.Clear();
            if (!Side(L, false, ZipRow[0]))
                return false;
            auto Put = [&](int A, int B, int D) { return ZipTris.Add(A) && ZipTris.Add(B) && ZipTris.Add(D); };
            const FV3 U = Place[C[L]], W = Place[C[(L + 1) % 3]], V = Place[C[(L + 2) % 3]];
            if (ZipRow[0].Num >= 3)
            {
                // The longest side one row, the other two the other: from the side's first corner to each row's next point,
                // then rung by rung along the side, and the last rung to its other corner.
                if (!Side((L + 2) % 3, true, ZipRow[1]) || !Side((L + 1) % 3, true, ZipRow[1]))
                    return false;
                const int    N0  = ZipRow[0].Num, N1 = ZipRow[1].Num;
                const FV3    Way = W - U;
                const double W2  = RdxSphere::Dot(Way, Way);
                auto At = [&](int I) { return RdxSphere::Dot(Out.Orig[I] - U, Way) / W2; };
                if (!Put(ZipRow[0][0], ZipRow[0][1], ZipRow[1][1]))
                    return false;
                int i = 1, j = 1;
                while (i < N0 - 2 || j < N1 - 2)
                {
                    const bool bLong = (j >= N1 - 2) || (i < N0 - 2 && At(ZipRow[0][i + 1]) <= At(ZipRow[1][j + 1]));
                    if (bLong ? !Put(ZipRow[0][i], ZipRow[0][i + 1], ZipRow[1][j])
                              : !Put(ZipRow[0][i], ZipRow[1][j + 1], ZipRow[1][j]))
                        return false;
                    i += bLong ? 1 : 0;
                    j += bLong ? 0 : 1;
                }
                if (!Put(ZipRow[0][N0 - 2], ZipRow[0][N0 - 1], ZipRow[1][N1 - 2]))
                    return false;
            }
            else
            {
                // The longest side not cut: the other two, each a row from it to their common corner, rung by rung away from
                // it - the rungs ordered by how far across the face from the longest side their points lie.
                ZipRow[0].Clear();
                if (!Side((L + 2) % 3, true, ZipRow[0]) || !Side((L + 1) % 3, false, ZipRow[1]))
                    return false;
                const int    M0   = ZipRow[0].Num, M1 = ZipRow[1].Num;
                const FV3    Base = W - U;
                const FV3    Up   = (V - U) - Base * (RdxSphere::Dot(V - U, Base) / RdxSphere::Dot(Base, Base));
                const double Up2  = RdxSphere::Dot(Up, Up);
                if (M0 < 2 || M1 < 2 || !(Up2 > 0.0))
                    return false;
                auto Across = [&](int I) { return RdxSphere::Dot(Out.Orig[I] - U, Up) / Up2; };
                int i = 0, j = 0;
                while (i < M0 - 2 || j < M1 - 2)
                {
                    const bool bFirst = (j >= M1 - 2) || (i < M0 - 2 && Across(ZipRow[0][i + 1]) <= Across(ZipRow[1][j + 1]));
                    if (bFirst ? !Put(ZipRow[0][i], ZipRow[1][j], ZipRow[0][i + 1])
                               : !Put(ZipRow[0][i], ZipRow[1][j], ZipRow[1][j + 1]))
                        return false;
                    i += bFirst ? 1 : 0;
                    j += bFirst ? 0 : 1;
                }
                if (!Put(ZipRow[0][M0 - 2], ZipRow[1][M1 - 2], ZipRow[0][M0 - 1]))
                    return false;
            }
            const FV3    Whole = RdxSphere::Cross(Place[C[1]] - Place[C[0]], Place[C[2]] - Place[C[0]]);
            const double Area  = RdxSphere::Length(Whole);
            if (!(Area > 0.0))
                return false;
            const FV3 Nf  = Whole * (1.0 / Area);
            double    Sum = 0.0;
            for (int t = 0; t < ZipTris.Num; t += 3)
            {
                const int    I0 = ZipTris[t], I1 = ZipTris[t + 1], I2 = ZipTris[t + 2];
                const FV3    A = Out.Orig[I0], B = Out.Orig[I1], D = Out.Orig[I2];
                const double Part = RdxSphere::Dot(RdxSphere::Cross(B - A, D - A), Nf);
                if (!(Part > Area * 1e-9))
                    return false;
                Sum += Part;
                const FV  Move = Surf.MoveAt(R, (A + B + D) * (1.0 / 3.0));
                const FV3 Lin  = ((Out.Moved[I0] - A) + (Out.Moved[I1] - B) + (Out.Moved[I2] - D)) * (1.0 / 3.0);
                if (fabs(((double)Move.X - Lin.X) * Nf.X + ((double)Move.Y - Lin.Y) * Nf.Y + ((double)Move.Z - Lin.Z) * Nf.Z) >
                    (double)Sag)
                    return false;
            }
            return fabs(Sum - Area) <= Area * 1e-6;
        };
        // A face laid on the surface, its triangles after all before: false where there was no memory for them.
        auto LayFace = [&](int f) -> bool
        {
            FaceZipped[f]        = 0;
            const int TrisBefore = TriPoly.Num, PlacesBefore = Out.Orig.Num;
            const int C[3] = { S.Weld[Corner[f * 3]], S.Weld[Corner[f * 3 + 1]], S.Weld[Corner[f * 3 + 2]] };
            int       EdgeOfFace[3], EdgeN[3];
            bool      bForward[3];
            int       N = 1;
            for (int e = 0; e < 3; e++)
            {
                EdgeOfFace[e] = S.EdgeOf[f * 3 + e];
                EdgeN[e]      = Cuts[EdgeOfFace[e]];
                bForward[e]   = C[e] == S.EdgeA[EdgeOfFace[e]];
                N             = (GridCuts[EdgeOfFace[e]] > N) ? GridCuts[EdgeOfFace[e]] : N;
            }
            const int R  = S.Poly[f];
            const FV3 P0 = Place[C[0]], P1 = Place[C[1]], P2 = Place[C[2]];
            // Its middle: where the surface stands off the flat triangle through its corners' moves - off its plane - by
            // more than Sag, the grid as fine as a bow of that height needs.
            if (N < MostCuts && Surf.Bends[R])
            {
                const FV    Lin = (PlaceMove[C[0]] + PlaceMove[C[1]] + PlaceMove[C[2]]) * (1.0f / 3.0f);
                const FV    D   = Surf.MoveAt(R, (P0 + P1 + P2) * (1.0 / 3.0)) - Lin;
                const FV3   Nf  = FaceNormal(Place, Corner, f);
                const float Off = fabsf((float)(D.X * Nf.X + D.Y * Nf.Y + D.Z * Nf.Z));
                if (Off > Sag)
                {
                    const int Need = (int)ceilf(sqrtf(Off / Sag));
                    N              = (Need > N) ? ((Need < MostCuts) ? Need : MostCuts) : N;
                }
            }
            // The point of edge e at step Step of Steps along this face's way: the nearest of the edge's own points - an edge
            // cut fewer times than the face takes the face's grid down to its pieces there, the grid's triangles left with
            // no area dropped, so that both sides of the edge have the same points on it and nothing else.
            auto EdgePointAt = [&](int e, int Step, int Steps)
            {
                const int Ed  = EdgeOfFace[e];
                const int Ne  = EdgeN[e];
                const int Num = bForward[e] ? Step : Steps - Step;
                const int k   = (Num * Ne * 2 + Steps) / (Steps * 2);
                return (k == 0) ? S.EdgeA[Ed] : (k == Ne) ? S.EdgeB[Ed] : EdgePoint[Ed] + k - 1;
            };
            if (!Grid.Resize((N + 1) * (N + 1)))
            {
                return false;
            }
            for (int i = 0; i <= N; i++)
                for (int j = 0; i + j <= N; j++)
                {
                    int Index = -1;
                    if (i == 0 && j == 0)
                        Index = C[0];
                    else if (i == N && j == 0)
                        Index = C[1];
                    else if (i == 0 && j == N)
                        Index = C[2];
                    else if (j == 0)
                        Index = EdgePointAt(0, i, N);
                    else if (i + j == N)
                        Index = EdgePointAt(1, j, N);
                    else if (i == 0)
                        Index = EdgePointAt(2, N - j, N);
                    else
                    {
                        const double B1 = (double)i / (double)N, B2 = (double)j / (double)N, B0 = 1.0 - B1 - B2;
                        const FV3    Flat = P0 * B0 + P1 * B1 + P2 * B2;
                        const FV     Move = Surf.MoveAt(R, Flat);
                        Index             = Out.Orig.Num;
                        if (!Out.Orig.Add(Flat) || !Out.Moved.Add(Flat + RdxSphere::V3(Move.X, Move.Y, Move.Z)))
                        {
                            return false;
                        }
                    }
                    Grid[i * (N + 1) + j] = Index;
                }
            // The grid's triangles - but not one two of whose corners an edge's coarser pieces took to one point.
            auto Emit = [&](int A, int B, int D) -> bool
            {
                if (A == B || B == D || D == A)
                    return true;
                return TriAt.Add(A) && TriAt.Add(B) && TriAt.Add(D) && TriPoly.Add(FaceOf[f]);
            };
            // A side whose edge has more points than the grid takes on it (cut close to its curve all along): each of its
            // points, the face's way round (SidePts), and a grid triangle along it laid as a fan from its middle round
            // those between its corners as well - both sides of the edge then have all of its points on it.
            bool bFan = false;
            for (int e = 0; e < 3; e++)
                bFan = bFan || EdgeN[e] > N;
            if (bFan)
                for (int e = 0; e < 3; e++)
                {
                    SidePts[e].Clear();
                    const int Ed = EdgeOfFace[e], Ne = EdgeN[e];
                    for (int s = 0; s <= Ne; s++)
                    {
                        const int k = bForward[e] ? s : Ne - s;
                        if (!SidePts[e].Add((k == 0) ? S.EdgeA[Ed] : (k == Ne) ? S.EdgeB[Ed] : EdgePoint[Ed] + k - 1))
                        {
                            return false;
                        }
                    }
                }
            auto Lay = [&](int A, int B, int D) -> bool
            {
                if (!bFan || A == B || B == D || D == A)
                    return Emit(A, B, D);
                const int Corners[3] = { A, B, D };
                FanRing.Clear();
                bool bMore = false;
                for (int c = 0; c < 3; c++)
                {
                    const int X = Corners[c], Y = Corners[(c + 1) % 3];
                    if (!FanRing.Add(X))
                        return false;
                    for (int e = 0; e < 3; e++)
                    {
                        int p = -1, q = -1;
                        for (int s = 0; s < SidePts[e].Num; s++)
                        {
                            p = (SidePts[e][s] == X) ? s : p;
                            q = (SidePts[e][s] == Y) ? s : q;
                        }
                        if (p < 0 || q < 0 || abs(p - q) < 2)
                            continue;
                        for (int s = p + ((q > p) ? 1 : -1); s != q; s += (q > p) ? 1 : -1)
                            if (!FanRing.Add(SidePts[e][s]))
                                return false;
                        bMore = true;
                        break;
                    }
                }
                if (!bMore)
                    return Emit(A, B, D);
                const FV3 Mid  = (Out.Orig[A] + Out.Orig[B] + Out.Orig[D]) * (1.0 / 3.0);
                const FV  Move = Surf.MoveAt(R, Mid);
                const int M    = Out.Orig.Num;
                if (!Out.Orig.Add(Mid) || !Out.Moved.Add(Mid + RdxSphere::V3(Move.X, Move.Y, Move.Z)))
                    return false;
                for (int r = 0; r < FanRing.Num; r++)
                    if (!Emit(FanRing[r], FanRing[(r + 1) % FanRing.Num], M))
                        return false;
                return true;
            };
            for (int i = 0; i < N; i++)
                for (int j = 0; i + j < N; j++)
                {
                    const int A = Grid[i * (N + 1) + j], B = Grid[(i + 1) * (N + 1) + j], D = Grid[i * (N + 1) + j + 1];
                    if (!Lay(A, B, D) || (i + j + 1 < N && !Lay(B, Grid[(i + 1) * (N + 1) + j + 1], D)))
                    {
                        return false;
                    }
                }
            // Where the grid turns some of the face over - a long thin face's grid is as thin as the face, and the surface's
            // least twist across it turns its triangles over - its sides' points zipped instead, where that turns less of it
            // over: the grid's triangles and its own points go.
            if (N > 1 && Surf.Bends[R])
            {
                const double Over = TurnedSince(TrisBefore);
                if (Over > 0.0 && Zip(R, C, EdgeOfFace, EdgeN, bForward))
                {
                    double ZipOver = 0.0;
                    for (int t = 0; t < ZipTris.Num; t += 3)
                    {
                        const int A = ZipTris[t], B = ZipTris[t + 1], D = ZipTris[t + 2];
                        const FV3 Now = RdxSphere::Cross(Out.Moved[B] - Out.Moved[A], Out.Moved[D] - Out.Moved[A]);
                        const FV3 Was = RdxSphere::Cross(Out.Orig[B] - Out.Orig[A], Out.Orig[D] - Out.Orig[A]);
                        if (RdxSphere::Dot(Now, Was) < 0.0)
                            ZipOver += 0.5 * RdxSphere::Length(Now);
                    }
                    if (ZipOver < Over)
                    {
                        TriAt.Num     = TrisBefore * 3;
                        TriPoly.Num   = TrisBefore;
                        Out.Orig.Num  = PlacesBefore;
                        Out.Moved.Num = PlacesBefore;
                        for (int t = 0; t < ZipTris.Num; t += 3)
                            if (!Emit(ZipTris[t], ZipTris[t + 1], ZipTris[t + 2]))
                            {
                                return false;
                            }
                        Out.Zipped++;
                        FaceZipped[f] = 1;
                    }
                }
            }
            return true;
        };
        for (int f = 0; f < Faces; f++)
        {
            // A face that turns over where it shows: taken back below.
            FaceTri0[f] = TriPoly.Num;
            if (!LayFace(f) || (TurnPasses > 1 && ShowsTurned(f, FaceTri0[f]) && !Turned.Add(f)))
            {
                delete Shape;
                return false;
            }
            FaceTriN[f] = TriPoly.Num - FaceTri0[f];
        }
        // Each face turned over where it shows: its edges cut more finely than their middles asked cut by them - their
        // points made again, as many as the faces' grids take - and every face on such an edge laid again, its triangles
        // before dropped (TriPoly -1); as often as that turns a face over, up to TurnPasses times in all.
        for (int Again = 1; Again < TurnPasses && Turned.Num > 0; Again++)
        {
            int Taken = 0;
            for (int i = 0; i < Turned.Num; i++)
                for (int e = 0; e < 3; e++)
                {
                    const int E = S.EdgeOf[Turned[i] * 3 + e];
                    if (!Finer[E] || Revert[E])
                        continue;
                    Revert[E]  = 1;
                    Finer[E]   = 0;
                    Changed[E] = 1;
                    Taken++;
                    const int a = S.EdgeA[E], b = S.EdgeB[E], N = GridCuts[E];
                    Cuts[E]      = N;
                    EdgePoint[E] = Out.Orig.Num;
                    for (int k = 1; k < N; k++)
                    {
                        const FV3 Flat = Place[a] + (Place[b] - Place[a]) * ((double)k / (double)N);
                        const FV  Move = Surf.MoveAt(EdgeRegion[E], Flat);
                        if (!Out.Orig.Add(Flat) || !Out.Moved.Add(Flat + RdxSphere::V3(Move.X, Move.Y, Move.Z)))
                        {
                            delete Shape;
                            return false;
                        }
                    }
                }
            Turned.Clear();
            if (Taken == 0)
                break;
            for (int f = 0; f < Faces; f++)
            {
                if (!Changed[S.EdgeOf[f * 3]] && !Changed[S.EdgeOf[f * 3 + 1]] && !Changed[S.EdgeOf[f * 3 + 2]])
                    continue;
                for (int t = FaceTri0[f]; t < FaceTri0[f] + FaceTriN[f]; t++)
                    TriPoly[t] = -1;
                Out.Zipped -= FaceZipped[f];
                FaceTri0[f] = TriPoly.Num;
                if (!LayFace(f) || (Again + 1 < TurnPasses && ShowsTurned(f, FaceTri0[f]) && !Turned.Add(f)))
                {
                    delete Shape;
                    return false;
                }
                FaceTriN[f] = TriPoly.Num - FaceTri0[f];
            }
            for (int E = 0; E < S.Edges; E++)
                Changed[E] = 0;
        }
        // The triangles dropped taken out; the points only they had stay, of no triangle.
        int Kept = 0;
        for (int t = 0; t < TriPoly.Num; t++)
            if (TriPoly[t] >= 0)
            {
                TriPoly[Kept] = TriPoly[t];
                for (int k = 0; k < 3; k++)
                    TriAt[Kept * 3 + k] = TriAt[t * 3 + k];
                Kept++;
            }
        TriPoly.Num = Kept;
        TriAt.Num   = Kept * 3;
    }
    if (!Out.PolyEdgeKinds.Resize(NumPolys * 5))
    {
        delete Shape;
        return false;
    }
    for (int i = 0; i < NumPolys * 5; i++)
        Out.PolyEdgeKinds[i] = 0;
    for (int f = 0; f < Faces; f++)
        for (int e = 0; e < 3; e++)
        {
            const int K = S.EdgeKind[S.EdgeOf[f * 3 + e]];
            if (K != RdxMeshRound::FShape::Edge_Inner && K >= 0 && K < 5)
                Out.PolyEdgeKinds[FaceOf[f] * 5 + K]++;
        }
    if (!LightSidesOf(S, Place, Corner, FaceOf, Faces, Ring, RingOwn, RingFirst, RingCount, PolyFace0, PolyFaces,
                      NumPolys, Cap, Out))
    {
        delete Shape;
        return false;
    }
    TList<int> PolyRegion;   // a polygon: its flat polygon
    if (!PolyRegion.Resize(NumPolys))
    {
        delete Shape;
        return false;
    }
    for (int p = 0; p < NumPolys; p++)
        PolyRegion[p] = (PolyFaces[p] > 0) ? S.Poly[PolyFace0[p]] : -1;
    delete Shape;
    // The wall held back where its round would come through a still polygon standing before it (HoldBehind).
    if (!HoldBehind(Points, Polys, NumPolys, TriPoly, TriAt, Cap, Out))
        return false;
    Lap(6);

    // 7. Each polygon's triangles together, for a polygon any point of which moved; the surface's turn round the
    //    upright at each corner, for the relief's axes.
    TList<unsigned char> Moves;
    if (!Moves.Resize(NumPolys))
        return false;
    for (int p = 0; p < NumPolys; p++)
        Moves[p] = 0;
    for (int t = 0; t < TriPoly.Num; t++)
        for (int k = 0; k < 3; k++)
        {
            const int    v = TriAt[t * 3 + k];
            const double D = RdxSphere::Length(Out.Moved[v] - Out.Orig[v]);
            if (D > 1e-4)
                Moves[TriPoly[t]] = 1;
            Out.LargestMove = (D > Out.LargestMove) ? D : Out.LargestMove;
        }
    // The triangles by polygon: counted, then laid out in polygon order.
    TList<int> Fill;
    if (!Fill.Resize(NumPolys))
        return false;
    for (int p = 0; p < NumPolys; p++)
        Fill[p] = 0;
    for (int t = 0; t < TriPoly.Num; t++)
        if (Moves[TriPoly[t]])
            Out.PolyTris[TriPoly[t]]++;
    int Next = 0;
    for (int p = 0; p < NumPolys; p++)
    {
        Out.PolyFirst[p] = Next;
        Fill[p]          = Next;
        Next += Out.PolyTris[p];
        Out.Touched += (Out.PolyTris[p] > 0) ? 1 : 0;
    }
    if (!Out.Tri.Resize(Next * 3) || !Out.TriTurn.Resize(Next))
        return false;
    // Where each triangle goes (-1: its polygon does not move), its corners put there.
    TList<int> TriOut;
    if (!TriOut.Resize(TriPoly.Num))
        return false;
    for (int t = 0; t < TriPoly.Num; t++)
    {
        const int p = TriPoly[t];
        TriOut[t]   = -1;
        if (!Moves[p])
            continue;
        const int o = Fill[p]++;
        TriOut[t]   = o;
        for (int k = 0; k < 3; k++)
            Out.Tri[o * 3 + k] = TriAt[t * 3 + k];
    }
    // Each one's turn round the upright, on threads: the moved triangle's normal against its polygon's, each laid flat;
    // none on a polygon that is no wall. A wall polygon's own heading is taken once by each thread for its triangles,
    // which come one after another.
    int    HeadingOf[RdxSphere::WorkersMost];
    double Heading[RdxSphere::WorkersMost];
    for (int w = 0; w < RdxSphere::WorkersMost; w++)
    {
        HeadingOf[w] = -1;
        Heading[w]   = 0.0;
    }
    RdxSphere::OnThreads(TriPoly.Num, 1024, Out.Workers, [&](int t, int w)
    {
        const int o = TriOut[t];
        if (o < 0)
            return;
        const int      p  = TriPoly[t];
        const FPolyIn& P  = Polys[p];
        float          Tu = 0.0f;
        if (fabs(P.Nz) < 0.7)
        {
            const FV3 A = Out.Moved[TriAt[t * 3]], B = Out.Moved[TriAt[t * 3 + 1]], C = Out.Moved[TriAt[t * 3 + 2]];
            const FV3 N = RdxSphere::Cross(B - A, C - A);
            if (RdxSphere::Dot(N, N) > 1e-12 && RdxSphere::Dot(N, RdxSphere::V3(P.Nx, P.Ny, P.Nz)) > 0.0)
            {
                if (HeadingOf[w] != p)
                {
                    HeadingOf[w] = p;
                    Heading[w]   = atan2(P.Ny, P.Nx);
                }
                Tu = (float)RdxSphere::Wrap(atan2(N.Y, N.X) - Heading[w]);
            }
        }
        Out.TriTurn[o] = Tu;
    });
    // The moved polygons' faces of the surface - its flat polygon's quad, else those of its pieces that overlap it, or for
    // a sliver of a polygon the one its middle lies on -, kept for what is laid on them, and the polygons by a grid over
    // the plan.
    if (!Out.PolyFaceFirst.Resize(NumPolys) || !Out.PolyFaceCount.Resize(NumPolys))
        return false;
    TList<FV3> RingAt;
    for (int p = 0; p < NumPolys; p++)
    {
        Out.PolyFaceFirst[p] = Out.FaceKind.Num;
        Out.PolyFaceCount[p] = 0;
        const int R = PolyRegion[p];
        if (!Moves[p] || R < 0 || RingCount[p] < 3)
            continue;
        RingAt.Clear();
        FV3 Middle = RdxSphere::V3(0.0, 0.0, 0.0);
        for (int i = 0; i < RingCount[p]; i++)
        {
            if (!RingAt.Add(Place[Ring[RingFirst[p] + i]]))
                return false;
            Middle = Middle + RingAt[i] * (1.0 / RingCount[p]);
        }
        const FV3 N = FaceNormal(Place, Corner, PolyFace0[p]);
        // A face: its kind, four corners and four edges' terms (a triangle's last ones its third corner and none).
        auto Face = [&](unsigned char Kind, const FV3* Q, const RdxMeshRound::FEdgeTerms* T, int Sides)
        {
            Out.PolyFaceCount[p]++;
            if (!Out.FaceKind.Add(Kind))
                return false;
            for (int k = 0; k < 4; k++)
                if (!Out.FaceCorner.Add(Q[(k < Sides) ? k : Sides - 1]))
                    return false;
            for (int e = 0; e < 4; e++)
                for (int k = 0; k < RdxMeshRound::EDGE_SLOTS; k++)
                {
                    const bool bHas = e < Sides;
                    if (!Out.FaceTerm.Add(bHas ? T[e].T[k].X : 0.0f) || !Out.FaceTerm.Add(bHas ? T[e].T[k].Y : 0.0f) ||
                        !Out.FaceTerm.Add(bHas ? T[e].T[k].Z : 0.0f) || !Out.FaceTerm.Add(bHas ? T[e].Room[k] : 1.0f))
                        return false;
                }
            return true;
        };
        auto Keep = [&](int i)
        {
            const FV3 T[3] = { Surf.A[i], Surf.A[i] + Surf.E1[i], Surf.A[i] + Surf.E2[i] };
            return Face(0, T, &Surf.Terms[i * 3], 3);
        };
        if (Surf.QuadOf[R] >= 0)
        {
            const int q = Surf.QuadOf[R];
            if (!Face((unsigned char)(1 + Surf.QuadDrop[q]), &Surf.QuadCorner[q * 4], &Surf.QuadSide[q * 4], 4))
                return false;
            continue;
        }
        FV3 Lo = RingAt[0], Hi = RingAt[0];
        for (int i = 1; i < RingAt.Num; i++)
        {
            Lo = RdxSphere::V3(MinOf(Lo.X, RingAt[i].X), MinOf(Lo.Y, RingAt[i].Y), MinOf(Lo.Z, RingAt[i].Z));
            Hi = RdxSphere::V3(MaxOf(Hi.X, RingAt[i].X), MaxOf(Hi.Y, RingAt[i].Y), MaxOf(Hi.Z, RingAt[i].Z));
        }
        for (int j = Surf.First[R]; j < Surf.First[R] + Surf.Count[R]; j++)
        {
            const int i = Surf.Items[j];
            if (Surf.Hi[i].X < Lo.X || Surf.Hi[i].Y < Lo.Y || Surf.Hi[i].Z < Lo.Z || Surf.Lo[i].X > Hi.X ||
                Surf.Lo[i].Y > Hi.Y || Surf.Lo[i].Z > Hi.Z)
                continue;
            const FV3 T[3] = { Surf.A[i], Surf.A[i] + Surf.E1[i], Surf.A[i] + Surf.E2[i] };
            if (Overlaps(T, RingAt.Data, RingAt.Num, N) && !Keep(i))
                return false;
        }
        if (Out.PolyFaceCount[p] == 0)
        {
            double    B[3];
            const int i = Surf.PieceAt(R, Middle, B);
            if (i >= 0 && !Keep(i))
                return false;
        }
    }
    {
        double X0 = 1e300, Y0 = 1e300, X1 = -1e300, Y1 = -1e300;
        for (int i = 0; i < Out.FaceCorner.Num; i++)
        {
            X0 = MinOf(X0, Out.FaceCorner[i].X);
            Y0 = MinOf(Y0, Out.FaceCorner[i].Y);
            X1 = MaxOf(X1, Out.FaceCorner[i].X);
            Y1 = MaxOf(Y1, Out.FaceCorner[i].Y);
        }
        if (Out.FaceCorner.Num > 0)
        {
            Out.PolyGridX0 = X0 - 1.0;
            Out.PolyGridY0 = Y0 - 1.0;
            Out.PolyGridW  = (int)((X1 - X0 + 2.0) / PolyCell) + 1;
            Out.PolyGridH  = (int)((Y1 - Y0 + 2.0) / PolyCell) + 1;
            if ((double)Out.PolyGridW * (double)Out.PolyGridH > 4.0e6)
                Out.PolyGridW = Out.PolyGridH = 0;
        }
        const int Cells = Out.PolyGridW * Out.PolyGridH;
        if (Cells > 0 && (!Out.PolyGridFirst.Resize(Cells) || !Out.PolyGridCount.Resize(Cells)))
            return false;
        for (int c = 0; c < Cells; c++)
            Out.PolyGridCount[c] = 0;
        // Twice over the moved polygons: counted into the cells their boxes cover, then filed.
        for (int Pass = 0; Pass < 2 && Cells > 0; Pass++)
        {
            if (Pass == 1)
            {
                int Next2 = 0;
                for (int c = 0; c < Cells; c++)
                {
                    Out.PolyGridFirst[c] = Next2;
                    Next2 += Out.PolyGridCount[c];
                    Out.PolyGridCount[c] = 0;
                }
                if (!Out.PolyGridItems.Resize(Next2))
                    return false;
            }
            for (int p = 0; p < NumPolys; p++)
            {
                if (Out.PolyFaceCount[p] == 0)
                    continue;
                double BX0 = 1e300, BY0 = 1e300, BX1 = -1e300, BY1 = -1e300;
                for (int i = Out.PolyFaceFirst[p] * 4; i < (Out.PolyFaceFirst[p] + Out.PolyFaceCount[p]) * 4; i++)
                {
                    BX0 = MinOf(BX0, Out.FaceCorner[i].X);
                    BY0 = MinOf(BY0, Out.FaceCorner[i].Y);
                    BX1 = MaxOf(BX1, Out.FaceCorner[i].X);
                    BY1 = MaxOf(BY1, Out.FaceCorner[i].Y);
                }
                const int CX0 = (int)((BX0 - 2.0 - Out.PolyGridX0) / PolyCell), CX1 = (int)((BX1 + 2.0 - Out.PolyGridX0) / PolyCell);
                const int CY0 = (int)((BY0 - 2.0 - Out.PolyGridY0) / PolyCell), CY1 = (int)((BY1 + 2.0 - Out.PolyGridY0) / PolyCell);
                for (int cy = (CY0 < 0 ? 0 : CY0); cy <= CY1 && cy < Out.PolyGridH; cy++)
                    for (int cx = (CX0 < 0 ? 0 : CX0); cx <= CX1 && cx < Out.PolyGridW; cx++)
                    {
                        const int c = cy * Out.PolyGridW + cx;
                        if (Pass == 1)
                            Out.PolyGridItems[Out.PolyGridFirst[c] + Out.PolyGridCount[c]] = p;
                        Out.PolyGridCount[c]++;
                    }
            }
        }
    }
    // What is kept for the level's time, cut down to what it holds - a list grown once to its length still has the room
    // its growing doubled up to.
    Out.PolyFirst.Shrink();
    Out.PolyTris.Shrink();
    Out.PolyFaceFirst.Shrink();
    Out.PolyFaceCount.Shrink();
    Out.PolyGridFirst.Shrink();
    Out.PolyGridCount.Shrink();
    Out.PolyEdgeKinds.Shrink();
    Out.PolySideFirst.Shrink();
    Out.PolySideCount.Shrink();
    Out.Orig.Shrink();
    Out.Moved.Shrink();
    Out.Tri.Shrink();
    Out.TriTurn.Shrink();
    Out.FaceKind.Shrink();
    Out.FaceCorner.Shrink();
    Out.FaceTerm.Shrink();
    Out.PolyGridItems.Shrink();
    Out.LightSides.Shrink();
    Out.LightParts.Shrink();
    Out.Left.Shrink();
    Lap(7);
    return true;
}

// The outlines of the moved walls the relief may be drawn on (Use_Relief) as their triangles draw them, into FResult's
// PolyChordFirst and the records after it. A polygon's triangles go round it the way its corners do, so the sides that
// no other of its triangles has, each from its start to its end, make one ring round the polygon: through its corners,
// the T-junctions put into its edges and every point its edges were cut at. Each corner is found on the ring, and the
// ring from one corner to the next is that edge's record. A polygon whose sides make no such ring - one with a hole, or
// touching itself - has none, and the relief's outline there keeps to the curve itself. False where there was no
// memory for it.
inline bool ChordsOf(const FV3* Points, int NumPoints, const FPolyIn* Polys, int NumPolys, FResult& Out)
{
    Out.PolyChordFirst.Clear();
    Out.ChordFirst.Clear();
    Out.ChordCount.Clear();
    Out.ChordEnds.Clear();
    Out.ChordT.Clear();
    Out.ChordMove.Clear();
    Out.ChordPolys = Out.ChordMissed = 0;
    if (!Out.PolyChordFirst.Resize(NumPolys))
        return false;
    // Each polygon's records worked out on threads into lists of the thread's own, and gathered after in the polygons'
    // order: as one after another would have made them. Per polygon: what it came to (0 no relief wall, 1 no ring, 2 its
    // records), whose lists hold them, where they start in them and how many.
    struct FWork
    {
        // The polygon's triangles' sides, in a table at least twice as large as their number: each side's start and end
        // (-1: a slot no side has) and how many of the triangles have it.
        TList<int>       SideA, SideB, SideCount;
        TList<long long> Rim;            // the sides whose reverse no triangle has: the start in the high half, the end low
        TList<int>       From, To;       // the ring's sides, by their starts in order
        TList<int>       At;             // a corner: where it lies on the ring
        TList<int>       First, Count;   // the records: their first points in T and Move, and how many
        TList<FV3>       Ends;           // two a record
        TList<float>     T;
        TList<FV3>       Move;
        bool             bMemory = true;
    };
    FWork      Work[RdxSphere::WorkersMost];
    TList<int> Got, Whose, RecFirst, RecCount;
    if (!Got.Resize(NumPolys) || !Whose.Resize(NumPolys) || !RecFirst.Resize(NumPolys) || !RecCount.Resize(NumPolys))
        return false;
    RdxSphere::OnThreads(NumPolys, 16, Out.Workers, [&](int p, int w)
    {
        FWork& K = Work[w];
        Got[p]   = 0;
        const FPolyIn& P = Polys[p];
        if (!K.bMemory || !(P.Use & RdxSphere::Use_Relief) || p >= Out.PolyTris.Num || Out.PolyTris[p] <= 0 ||
            P.Count < 3 || P.First < 0 || P.First + P.Count > NumPoints)
            return;
        int Size = 16;
        while (Size < 6 * Out.PolyTris[p])
            Size *= 2;
        // The slot of the side from A to B: its own, or the free one it would take.
        auto Slot = [&](int A, int B)
        {
            unsigned int H = (unsigned int)A * 0x9E3779B1u ^ (unsigned int)B * 0x85EBCA77u;
            H ^= H >> 15;
            int s = (int)(H & (unsigned int)(Size - 1));
            while (K.SideA[s] >= 0 && (K.SideA[s] != A || K.SideB[s] != B))
                s = (s + 1) & (Size - 1);
            return s;
        };
        auto NextOf = [&](int v)
        {
            int Lo = 0, Hi = K.From.Num - 1;
            while (Lo <= Hi)
            {
                const int Mid = (Lo + Hi) / 2;
                if (K.From[Mid] == v)
                    return K.To[Mid];
                if (K.From[Mid] < v)
                    Lo = Mid + 1;
                else
                    Hi = Mid - 1;
            }
            return -1;
        };
        if (!K.SideA.Resize(Size) || !K.SideB.Resize(Size) || !K.SideCount.Resize(Size))
        {
            K.bMemory = false;
            return;
        }
        for (int s = 0; s < Size; s++)
            K.SideA[s] = -1;
        for (int t = Out.PolyFirst[p]; t < Out.PolyFirst[p] + Out.PolyTris[p]; t++)
            for (int k = 0; k < 3; k++)
            {
                const int A = Out.Tri[t * 3 + k], B = Out.Tri[t * 3 + (k + 1) % 3];
                const int s = Slot(A, B);
                if (K.SideA[s] < 0)
                {
                    K.SideA[s]     = A;
                    K.SideB[s]     = B;
                    K.SideCount[s] = 0;
                }
                K.SideCount[s]++;
            }
        // The ring: the sides whose reverse no triangle of the polygon has, by their starts. A side twice, or a place two
        // of them start at, is no ring.
        K.Rim.Clear();
        bool bRing = true;
        for (int s = 0; s < Size && bRing; s++)
        {
            if (K.SideA[s] < 0 || K.SideA[Slot(K.SideB[s], K.SideA[s])] >= 0)
                continue;
            bRing = K.SideCount[s] == 1;
            if (bRing && !K.Rim.Add(((long long)K.SideA[s] << 32) | (long long)K.SideB[s]))
            {
                K.bMemory = false;
                return;
            }
        }
        if (bRing)
            Detail::SortSides(K.Rim.Data, K.Rim.Num);
        K.From.Clear();
        K.To.Clear();
        for (int i = 0; i < K.Rim.Num && bRing; i++)
        {
            const int A = (int)(K.Rim[i] >> 32), B = (int)(K.Rim[i] & 0xffffffffLL);
            bRing = K.From.Num == 0 || K.From[K.From.Num - 1] != A;
            if (bRing && (!K.From.Add(A) || !K.To.Add(B)))
            {
                K.bMemory = false;
                return;
            }
        }
        Got[p] = 1;
        if (!bRing || K.From.Num < 3)
            return;
        // Each corner's place on the ring: the welded place it went into.
        K.At.Clear();
        for (int k = 0; k < P.Count && bRing; k++)
        {
            const FV3& Q    = Points[P.First + k];
            int        Best = -1;
            double     Near = 4.0 * WeldDist * WeldDist;
            for (int i = 0; i < K.From.Num; i++)
            {
                const double D = Detail::Dist2(Out.Orig[K.From[i]], Q);
                if (D <= Near)
                {
                    Near = D;
                    Best = K.From[i];
                }
            }
            bRing = Best >= 0;
            if (bRing && !K.At.Add(Best))
            {
                K.bMemory = false;
                return;
            }
        }
        if (!bRing)
            return;
        // The ring from each corner to the next, once round: every side of it taken, each once.
        const int First = K.First.Num, PointsBefore = K.T.Num;
        int       Steps = 0;
        for (int k = 0; k < P.Count && bRing; k++)
        {
            const int    Start = K.At[k], End = K.At[(k + 1) % P.Count];
            const FV3    A = Points[P.First + k], B = Points[P.First + (k + 1) % P.Count];
            const FV3    AB = B - A;
            const double L2 = RdxSphere::Dot(AB, AB);
            if (!K.First.Add(K.T.Num) || !K.Ends.Add(A) || !K.Ends.Add(B) || !K.T.Add(0.0f) ||
                !K.Move.Add(Out.Moved[Start] - Out.Orig[Start]))
            {
                K.bMemory = false;
                return;
            }
            int   v    = Start;
            float Last = 0.0f;
            while (bRing && (v != End || K.T.Num - K.First[K.First.Num - 1] < 2))
            {
                if (v == End)
                {
                    // An edge whose two corners were welded into one place: its record is that place, twice.
                    if (!K.T.Add(1.0f) || !K.Move.Add(Out.Moved[v] - Out.Orig[v]))
                    {
                        K.bMemory = false;
                        return;
                    }
                    break;
                }
                v     = NextOf(v);
                bRing = v >= 0 && ++Steps <= K.From.Num;
                if (!bRing)
                    break;
                double T = (v == End) ? 1.0 : (L2 > 1e-12) ? RdxSphere::Dot(Out.Orig[v] - A, AB) / L2 : 0.0;
                T        = (T < 0.0) ? 0.0 : (T > 1.0) ? 1.0 : T;
                bRing    = (float)T >= Last;
                Last     = (float)T;
                if (bRing && (!K.T.Add((float)T) || !K.Move.Add(Out.Moved[v] - Out.Orig[v])))
                {
                    K.bMemory = false;
                    return;
                }
            }
            if (!K.Count.Add(K.T.Num - K.First[K.First.Num - 1]))
            {
                K.bMemory = false;
                return;
            }
        }
        if (!bRing || Steps != K.From.Num)
        {
            K.First.Num = K.Count.Num = First;
            K.Ends.Num  = 2 * First;
            K.T.Num = K.Move.Num = PointsBefore;
            return;
        }
        Got[p]      = 2;
        Whose[p]    = w;
        RecFirst[p] = First;
        RecCount[p] = K.First.Num - First;
    });
    for (int w = 0; w < RdxSphere::WorkersMost; w++)
        if (!Work[w].bMemory)
            return false;
    for (int p = 0; p < NumPolys; p++)
    {
        Out.PolyChordFirst[p] = -1;
        if (Got[p] == 1)
            Out.ChordMissed++;
        if (Got[p] != 2)
            continue;
        const FWork& K        = Work[Whose[p]];
        Out.PolyChordFirst[p] = Out.ChordFirst.Num;
        for (int r = RecFirst[p]; r < RecFirst[p] + RecCount[p]; r++)
        {
            if (!Out.ChordFirst.Add(Out.ChordT.Num) || !Out.ChordCount.Add(K.Count[r]) ||
                !Out.ChordEnds.Add(K.Ends[r * 2]) || !Out.ChordEnds.Add(K.Ends[r * 2 + 1]))
                return false;
            for (int i = K.First[r]; i < K.First[r] + K.Count[r]; i++)
                if (!Out.ChordT.Add(K.T[i]) || !Out.ChordMove.Add(K.Move[i]))
                    return false;
        }
        Out.ChordPolys++;
    }
    Out.PolyChordFirst.Shrink();
    Out.ChordFirst.Shrink();
    Out.ChordCount.Shrink();
    Out.ChordEnds.Shrink();
    Out.ChordT.Shrink();
    Out.ChordMove.Shrink();
    return true;
}

// The level rounded (RoundOnce), and the relief's outlines read off its triangles (ChordsOf) - none of an earlier
// rounding's left where this one fails.
inline bool Round(const FV3* Points, int NumPoints, const FPolyIn* Polys, int NumPolys, double Cap, double Angle,
                  FResult& Out)
{
    Out.PolyChordFirst.Clear();
    Out.ChordFirst.Clear();
    Out.ChordCount.Clear();
    Out.ChordEnds.Clear();
    Out.ChordT.Clear();
    Out.ChordMove.Clear();
    Out.ChordPolys = Out.ChordMissed = 0;
    if (!RoundOnce(Points, NumPoints, Polys, NumPolys, Cap, Angle, Out))
        return false;
    const double T0      = Out.Seconds ? Out.Seconds() : 0.0;
    const bool   bChords = ChordsOf(Points, NumPoints, Polys, NumPolys, Out);
    if (Out.Seconds)
        Out.PartMs[8] = (Out.Seconds() - T0) * 1000.0;
    return bChords;
}


// Where point P moves on a face of the rounded surface (FResult's FaceKind, FaceCorner, FaceTerm) - Kind 0 a triangle
// of corners Q[0..2], moved as its patch moves its point (MeshRound.h's PatchMoveAt); 1 + the axis its plane is laid
// flat along for a quad of corners Q[0..3] the way round, moved as a Coons patch (QuadMoveAt) -, held to Cap: false
// where P lies further than Near off its plane, or off it.
// Where P lies on a face (FaceMoveAt's first half): a triangle's corners' shares (S[0..2]), a quad's U and V (S[0], S[1]),
// each held within the face; and (Off, Nf, where asked) how far off the face's plane it lies and the plane's normal, of
// length one. False where it lies off the face's plane by more than Near, or outside the face.
inline bool FaceSharesAt(int Kind, const FV3* Q, const FV3& P, double Near, float* S, double* Off = nullptr,
                         FV3* Nf = nullptr)
{
    if (Kind == 0)
    {
        const FV3    E1 = Q[1] - Q[0], E2 = Q[2] - Q[0], D = P - Q[0];
        const FV3    N  = RdxSphere::Cross(E1, E2);
        const double NL = RdxSphere::Length(N);
        if (!(NL > 1e-12) || fabs(RdxSphere::Dot(D, N)) / NL > Near)
            return false;
        if (Off)
            *Off = fabs(RdxSphere::Dot(D, N)) / NL;
        if (Nf)
            *Nf = N * (1.0 / NL);
        const double D00 = RdxSphere::Dot(E1, E1), D01 = RdxSphere::Dot(E1, E2), D11 = RdxSphere::Dot(E2, E2);
        const double D20 = RdxSphere::Dot(D, E1), D21 = RdxSphere::Dot(D, E2);
        const double Den = D00 * D11 - D01 * D01;
        if (!(fabs(Den) > 1e-12))
            return false;
        const double B1 = (D11 * D20 - D01 * D21) / Den, B2 = (D00 * D21 - D01 * D20) / Den, B0 = 1.0 - B1 - B2;
        if (B0 < -1e-3 || B1 < -1e-3 || B2 < -1e-3)
            return false;
        S[0] = (float)MaxOf(B0, 0.0);
        S[1] = (float)MaxOf(B1, 0.0);
        S[2] = (float)MaxOf(B2, 0.0);
        return true;
    }
    const FV3    N  = RdxSphere::Cross(Q[1] - Q[0], Q[3] - Q[0]);
    const double NL = RdxSphere::Length(N);
    double       U = 0.0, V = 0.0;
    if (!(NL > 1e-12) || fabs(RdxSphere::Dot(P - Q[0], N)) / NL > Near || !Detail::QuadUvOf(Q, Kind - 1, P, U, V) ||
        U < -1e-3 || U > 1.0 + 1e-3 || V < -1e-3 || V > 1.0 + 1e-3)
        return false;
    if (Off)
        *Off = fabs(RdxSphere::Dot(P - Q[0], N)) / NL;
    if (Nf)
        *Nf = N * (1.0 / NL);
    S[0] = (float)MinOf(MaxOf(U, 0.0), 1.0);
    S[1] = (float)MinOf(MaxOf(V, 0.0), 1.0);
    return true;
}

// The face's move at the place FaceSharesAt found there (S), held to the cap (FaceMoveAt's second half).
inline void FaceMoveFrom(int Kind, const RdxMeshRound::FEdgeTerms* T, const float* S, double Cap, FV3& Move)
{
    RdxMeshRound::FV M = (Kind == 0) ? RdxMeshRound::PatchMoveAt(T, S[0], S[1], S[2]) : Detail::QuadMoveAt(T, S[0], S[1]);
    const float      L = RdxMeshRound::Length(M);
    if (L > (float)Cap && L > 0.0f)
        M = M * ((float)Cap / L);
    Move = RdxSphere::V3(M.X, M.Y, M.Z);
}

inline bool FaceMoveAt(int Kind, const FV3* Q, const RdxMeshRound::FEdgeTerms* T, const FV3& P, double Near, double Cap,
                       FV3& Move)
{
    float S[3] = { 0.0f, 0.0f, 0.0f };
    if (!FaceSharesAt(Kind, Q, P, Near, S))
        return false;
    FaceMoveFrom(Kind, T, S, Cap, Move);
    return true;
}

// Face f's edges' terms as FResult keeps them (FaceTerm), four edges.
inline void FaceTermsOf(const float* In, RdxMeshRound::FEdgeTerms* T)
{
    for (int e = 0; e < 4; e++)
        for (int k = 0; k < RdxMeshRound::EDGE_SLOTS; k++)
        {
            const float* const S = In + (e * RdxMeshRound::EDGE_SLOTS + k) * 4;
            T[e].T[k]            = RdxMeshRound::V(S[0], S[1], S[2]);
            T[e].Room[k]         = S[3];
        }
}

// Where a point laid on polygon Poly moves with it - P in the level's space, within Near of one of the polygon's faces
// of the rounded surface - into Move (FaceMoveAt). False where the polygon did not move, or the point lies on none of
// its faces.
inline bool MoveOnPolygon(const FResult& R, int Poly, const FV3& P, double Near, FV3& Move)
{
    if (Poly < 0 || Poly >= R.PolyFaceCount.Num || R.PolyFaceCount[Poly] <= 0)
        return false;
    for (int f = R.PolyFaceFirst[Poly]; f < R.PolyFaceFirst[Poly] + R.PolyFaceCount[Poly]; f++)
    {
        // The terms only for the face the point lies on: most faces looked at are not it.
        float S[3] = { 0.0f, 0.0f, 0.0f };
        if (!FaceSharesAt(R.FaceKind[f], &R.FaceCorner[f * 4], P, Near, S))
            continue;
        RdxMeshRound::FEdgeTerms T[4];
        FaceTermsOf(&R.FaceTerm[f * FaceTermFloats], T);
        FaceMoveFrom(R.FaceKind[f], T, S, R.MoveCap, Move);
        return true;
    }
    return false;
}

// Where places of polygon Poly were held back before a still polygon (HoldBehind), a point laid on it - a decal, the
// laser sight's spot - goes as the drawn triangle it lies on goes there: between its corners' moves, by its shares of
// the triangle where it was. The wall is held off the faces' curve there, and what lies on it has to stay on it. Move
// (MoveOnPolygon's) is left as it is on a triangle with no held corner, and where the point lies on none.
inline void HoldOnPolygon(const FResult& R, int Poly, const FV3& P, FV3& Move)
{
    if (Poly < 0 || Poly >= R.PolyHeld.Num || !R.PolyHeld[Poly])
        return;
    for (int t = R.PolyFirst[Poly]; t < R.PolyFirst[Poly] + R.PolyTris[Poly]; t++)
    {
        const int    a = R.Tri[t * 3], b = R.Tri[t * 3 + 1], c = R.Tri[t * 3 + 2];
        const FV3    E1 = R.Orig[b] - R.Orig[a], E2 = R.Orig[c] - R.Orig[a], D = P - R.Orig[a];
        const double D00 = RdxSphere::Dot(E1, E1), D01 = RdxSphere::Dot(E1, E2), D11 = RdxSphere::Dot(E2, E2);
        const double D20 = RdxSphere::Dot(D, E1), D21 = RdxSphere::Dot(D, E2), Den = D00 * D11 - D01 * D01;
        if (!(fabs(Den) > 1e-12))
            continue;
        const double B1 = (D11 * D20 - D01 * D21) / Den, B2 = (D00 * D21 - D01 * D20) / Den, B0 = 1.0 - B1 - B2;
        if (B0 < -1e-3 || B1 < -1e-3 || B2 < -1e-3)
            continue;
        if (R.PlaceHeld[a] || R.PlaceHeld[b] || R.PlaceHeld[c])
            Move = (R.Moved[a] - R.Orig[a]) * B0 + (R.Moved[b] - R.Orig[b]) * B1 + (R.Moved[c] - R.Orig[c]) * B2;
        return;
    }
}

// The rounding for the card (FSolidPass::SetSphereField; SolidPass.hlsl's SphereMoveAt reads it as MoveOnPolygon and
// PolygonAt read the result, and SphereChordAt the relief's outlines), in 32-bit words: a header of RoundHeader words -
// the plan grid's width and height, its corner, its cell, the cap, where the cells, their items, the polygons and the
// faces start, how many polygons and faces there are, where the outlines' records start and how many there are, and
// where their points start; then each cell's first item and count; the items, polygons; each polygon's first face and
// count; each face's kind, four corners and four edges' terms (FaceKind, FaceCorner, FaceTerm), RoundFaceWords words a
// face; each outline record's first point and count and its two ends, RoundChordWords words; and each point's place
// along its edge and its move, RoundPointWords words.
// Then the relief's floor: the triangles the moved polygons the relief may be drawn on (PolyRelief) are drawn with,
// for the relief to lie on them rather than on the faces' curve, which they meet only at their corners - a bend near
// the end of an edge the triangles keep straight (LevelRoundCheck's cover) left the relief up to the cap behind the
// surface drawn under it, and a turn of the faces' normal a step off it. The header's last seven words: where each
// polygon's record of them starts, and a plan grid of those polygons of its own, by their triangles' boxes - the faces'
// grid lists a polygon only where its faces reach: its width and height, its corner, where its cells and their items
// start (each cell's first item and count, the items polygons). Per polygon the word its record starts at, 0 none. A
// record, RoundPieceHeader words: its plane (normal and distance), the axis its plan is laid flat along, a grid over
// that plan - its width and height in one word, its corner and one over its cell's sides - and where its places, its
// triangles and its items start, from the record's start; then each cell's first item and count, sixteen bits each; a
// place, RoundPlaceWords words: where it was in the plan, how far it moved, three half floats, and the drawn surface's
// normal there (PlaceNormalOf), three more; a triangle its three places, sixteen bits each, in two words; the items the
// triangles over each cell, sixteen bits each, two to a word (SolidPass.hlsl's SpherePieceMoveAt, PackedPieceMoveAt). A
// large rounded level's floor is tens of megabytes even so.
static const int RoundHeader      = 22;
static const int RoundFaceWords   = 1 + 12 + FaceTermFloats;
static const int RoundChordWords  = 8;
static const int RoundPointWords  = 4;
static const int RoundPieceHeader = 13;
static const int RoundPlaceWords  = 5;
// How many triangles a cell of a polygon's grid takes, on average, and the most cells across.
static const int RoundPiecesPerCell = 8, RoundPieceCellsMost = 64;
// The plan a polygon's triangles are laid flat in: the axis its normal is most along is left out, the two others kept
// in order.
inline int PieceDropOf(const FV3& N)
{
    const double X = fabs(N.X), Y = fabs(N.Y), Z = fabs(N.Z);
    return (X >= Y && X >= Z) ? 0 : (Y >= Z) ? 1 : 2;
}
inline void PiecePlanOf(const FV3& P, int Drop, double& A, double& B)
{
    A = (Drop == 0) ? P.Y : P.X;
    B = (Drop == 2) ? P.Y : P.Z;
}
// The cells of a polygon's grid (PackRound's relief floor) the plan's box of triangle t reaches, from C0 to C1, both in -
// a little over the box, for a place on the triangle's side to find it from the cell beside, and held to the grid.
static const double RoundPiecePad = 0.05;
inline void PieceCellsOf(const FResult& R, int t, int Drop, double X0, double Y0, double InvX, double InvY, int Gx,
                         int Gy, int* C0, int* C1)
{
    double Lo[2] = { 1e300, 1e300 }, Hi[2] = { -1e300, -1e300 };
    for (int k = 0; k < 3; k++)
    {
        double A, B;
        PiecePlanOf(R.Orig[R.Tri[t * 3 + k]], Drop, A, B);
        Lo[0] = MinOf(Lo[0], A);
        Hi[0] = MaxOf(Hi[0], A);
        Lo[1] = MinOf(Lo[1], B);
        Hi[1] = MaxOf(Hi[1], B);
    }
    C0[0] = (int)MinOf(MaxOf(floor((Lo[0] - RoundPiecePad - X0) * InvX), 0.0), (double)(Gx - 1));
    C0[1] = (int)MinOf(MaxOf(floor((Lo[1] - RoundPiecePad - Y0) * InvY), 0.0), (double)(Gy - 1));
    C1[0] = (int)MinOf(MaxOf(floor((Hi[0] + RoundPiecePad - X0) * InvX), 0.0), (double)(Gx - 1));
    C1[1] = (int)MinOf(MaxOf(floor((Hi[1] + RoundPiecePad - Y0) * InvY), 0.0), (double)(Gy - 1));
}
// A float as a half float's sixteen bits, to nearest: what a floor's place moved by - a few units at most, a
// thousandth of it kept. Too small for a half's own exponent: none.
inline unsigned int HalfOf(double V)
{
    const float  F = (float)V;
    unsigned int U;
    memcpy(&U, &F, sizeof(U));
    const unsigned int Sign = (U >> 16) & 0x8000u;
    const int          Exp  = (int)((U >> 23) & 0xFFu) - 127 + 15;
    const unsigned int Man  = U & 0x7FFFFFu;
    if (Exp <= 0)
        return Sign;
    if (Exp >= 31)
        return Sign | 0x7BFFu;
    unsigned int       H    = Sign | ((unsigned int)Exp << 10) | (Man >> 13);
    const unsigned int Rest = Man & 0x1FFFu;
    if (Rest > 0x1000u || (Rest == 0x1000u && (H & 1u)))
        H++;
    return H;
}
inline float FloatOfHalf(unsigned int H)
{
    const unsigned int Exp = (H >> 10) & 0x1Fu;
    if (Exp == 0)
        return 0.0f;
    const unsigned int U = ((H & 0x8000u) << 16) | ((Exp - 15 + 127) << 23) | ((H & 0x3FFu) << 13);
    float              F;
    memcpy(&F, &U, sizeof(F));
    return F;
}
// The drawn surface's normal at place v of polygon p's floor - the way the relief is lifted there (DSRelief), so that
// on a rounded corner it stands out of the surface as drawn, not out of the wall's plane, which there runs along the
// surface and smears the relief across it: the drawn triangles round the place added up, each by its area (its
// normal's length), of p's own and of the polygons beside it that face within a relief's bend of p's plane (the
// renderer's ReliefBendCos) - in its plane beside it, or a face of a round tower or a column -, so that the polygons
// sharing a place there lift it alike; the far wall of a corner turns away from it and is left out. Each triangle
// turned to its polygon's front (Sign: its outline's way round against its plane). The plane's own where they add up
// to none or face away from it. PlaceFirst and PlaceTris: each place's triangles, TriPoly each triangle's polygon, PolyN
// each polygon's plane's normal (none where it has no triangles).
static const double RoundBendCos = 0.69;
inline FV3 PlaceNormalOf(const FResult& R, int p, int v, const TList<int>& PlaceFirst, const TList<int>& PlaceTris,
                         const TList<int>& TriPoly, const TList<FV3>& PolyN, const TList<int>& Sign)
{
    FV3 Sum = RdxSphere::V3(0.0, 0.0, 0.0);
    for (int i = PlaceFirst[v]; i < PlaceFirst[v + 1]; i++)
    {
        const int u = PlaceTris[i], q = TriPoly[u];
        if (RdxSphere::Dot(PolyN[q], PolyN[p]) < RoundBendCos)
            continue;
        const FV3& A = R.Moved[R.Tri[u * 3]];
        Sum          = Sum + RdxSphere::Cross(R.Moved[R.Tri[u * 3 + 1]] - A, R.Moved[R.Tri[u * 3 + 2]] - A) * (double)Sign[q];
    }
    const double L = RdxSphere::Length(Sum);
    if (!(L > 1e-12) || RdxSphere::Dot(Sum, PolyN[p]) <= 0.0)
        return PolyN[p];
    return Sum * (1.0 / L);
}
inline bool PackRound(const FResult& R, TList<unsigned int>& Out)
{
    const int Cells    = R.PolyGridW * R.PolyGridH;
    const int Polys    = R.PolyFaceCount.Num;
    const int Faces    = R.FaceKind.Num;
    const int Chords   = R.ChordFirst.Num;
    const int CellsAt  = RoundHeader, ItemsAt = CellsAt + 2 * Cells, PolysAt = ItemsAt + R.PolyGridItems.Num,
              FacesAt  = PolysAt + 2 * Polys;
    const int ChordsAt = FacesAt + RoundFaceWords * Faces, PointsAt = ChordsAt + RoundChordWords * Chords;
    // The relief's floor: which polygons, each one's grid, its places and items, counted first.
    struct FPieceGrid
    {
        FV3    N;
        double D, X0, Y0, InvX, InvY;
        int    Drop, Gx, Gy, Places, Items, Words;
    };
    TList<FPieceGrid> Grid;
    TList<int>        Local, Touched;
    if (!Grid.Resize(Polys) || !Local.Resize(R.Orig.Num))
        return false;
    for (int v = 0; v < R.Orig.Num; v++)
        Local[v] = -1;
    int PieceWords = 0;
    for (int p = 0; p < Polys; p++)
    {
        FPieceGrid& G = Grid[p];
        G.Gx          = 0;
        if (p >= R.PolyRelief.Num || !R.PolyRelief[p] || p >= R.PolyTris.Num || R.PolyTris[p] <= 0 ||
            R.PolyFaceCount[p] <= 0)
            continue;
        // The plane: the polygon's own as it came in; where that is not had, its triangles' normals where they were,
        // added up.
        FV3    N     = RdxSphere::V3(0.0, 0.0, 0.0);
        double Plane = 0.0;
        if (R.PolyPlane.Num >= 4 * Polys)
        {
            N     = RdxSphere::V3(R.PolyPlane[4 * p], R.PolyPlane[4 * p + 1], R.PolyPlane[4 * p + 2]);
            Plane = R.PolyPlane[4 * p + 3];
        }
        else
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                const FV3& A = R.Orig[R.Tri[t * 3]];
                N            = N + RdxSphere::Cross(R.Orig[R.Tri[t * 3 + 1]] - A, R.Orig[R.Tri[t * 3 + 2]] - A);
            }
        const double L = RdxSphere::Length(N);
        if (!(L > 1e-12))
            continue;
        const FV3 Nu = N * (1.0 / L);
        if (R.PolyPlane.Num < 4 * Polys)
            Plane = RdxSphere::Dot(Nu, R.Orig[R.Tri[R.PolyFirst[p] * 3]]);
        else
            Plane /= L;
        const int Drop = PieceDropOf(Nu);
        double    Lo[2] = { 1e300, 1e300 }, Hi[2] = { -1e300, -1e300 };
        for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            for (int k = 0; k < 3; k++)
            {
                double A, B;
                PiecePlanOf(R.Orig[R.Tri[t * 3 + k]], Drop, A, B);
                Lo[0] = MinOf(Lo[0], A);
                Hi[0] = MaxOf(Hi[0], A);
                Lo[1] = MinOf(Lo[1], B);
                Hi[1] = MaxOf(Hi[1], B);
            }
        const double W = MaxOf(Hi[0] - Lo[0], 1e-3), H = MaxOf(Hi[1] - Lo[1], 1e-3);
        const double C = MaxOf((double)R.PolyTris[p] / RoundPiecesPerCell, 1.0);
        const int    Gx = (int)MinOf(MaxOf(ceil(sqrt(C * W / H)), 1.0), (double)RoundPieceCellsMost);
        const int    Gy = (int)MinOf(MaxOf(ceil(sqrt(C * H / W)), 1.0), (double)RoundPieceCellsMost);
        // Its places, each once, and each triangle over every cell its plan's box reaches.
        int Places = 0, Items = 0;
        for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
        {
            int C0[2], C1[2];
            PieceCellsOf(R, t, Drop, Lo[0], Lo[1], Gx / W, Gy / H, Gx, Gy, C0, C1);
            Items += (C1[0] - C0[0] + 1) * (C1[1] - C0[1] + 1);
            for (int k = 0; k < 3; k++)
            {
                const int v = R.Tri[t * 3 + k];
                if (Local[v] >= 0)
                    continue;
                Local[v] = Places++;
                if (!Touched.Add(v))
                    return false;
            }
        }
        for (int i = 0; i < Touched.Num; i++)
            Local[Touched[i]] = -1;
        Touched.Clear();
        // Sixteen bits name a place, a triangle and an item within a record.
        if (Places > 0xFFFF || R.PolyTris[p] > 0xFFFF || Items > 0xFFFF)
            continue;
        G.N      = Nu;
        G.D      = Plane;
        G.Drop   = Drop;
        G.Gx     = Gx;
        G.Gy     = Gy;
        G.X0     = Lo[0];
        G.Y0     = Lo[1];
        G.InvX   = Gx / W;
        G.InvY   = Gy / H;
        G.Places = Places;
        G.Items  = Items;
        G.Words  = RoundPieceHeader + Gx * Gy + RoundPlaceWords * Places + 2 * R.PolyTris[p] + (Items + 1) / 2;
        PieceWords += G.Words;
    }
    // Their own plan grid, over their triangles' boxes and as far again as a place may lie off a polygon's plane.
    double        PX0 = 1e300, PY0 = 1e300, PX1 = -1e300, PY1 = -1e300;
    TList<double> Box;
    if (!Box.Resize(4 * ((Polys > 1) ? Polys : 1)))
        return false;
    for (int p = 0; p < Polys; p++)
    {
        if (Grid[p].Gx <= 0)
            continue;
        double* const B = &Box[4 * p];
        B[0] = B[1] = 1e300;
        B[2] = B[3] = -1e300;
        for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            for (int k = 0; k < 3; k++)
            {
                const FV3& V = R.Orig[R.Tri[t * 3 + k]];
                B[0]         = MinOf(B[0], V.X - 2.0);
                B[1]         = MinOf(B[1], V.Y - 2.0);
                B[2]         = MaxOf(B[2], V.X + 2.0);
                B[3]         = MaxOf(B[3], V.Y + 2.0);
            }
        PX0 = MinOf(PX0, B[0]);
        PY0 = MinOf(PY0, B[1]);
        PX1 = MaxOf(PX1, B[2]);
        PY1 = MaxOf(PY1, B[3]);
    }
    int PGW = 0, PGH = 0;
    if (PX1 >= PX0)
    {
        PGW = (int)((PX1 - PX0) / PolyCell) + 1;
        PGH = (int)((PY1 - PY0) / PolyCell) + 1;
        if ((double)PGW * (double)PGH > 4.0e6)
            PGW = PGH = 0;
    }
    TList<int> PCount;
    if (!PCount.Resize((PGW * PGH > 1) ? PGW * PGH : 1))
        return false;
    for (int c = 0; c < PGW * PGH; c++)
        PCount[c] = 0;
    int  PItems    = 0;
    auto PlanCells = [&](int p, int& CX0, int& CY0, int& CX1, int& CY1)
    {
        const double* const B = &Box[4 * p];
        CX0                   = (int)MaxOf(floor((B[0] - PX0) / PolyCell), 0.0);
        CY0                   = (int)MaxOf(floor((B[1] - PY0) / PolyCell), 0.0);
        CX1                   = (int)MinOf(floor((B[2] - PX0) / PolyCell), (double)(PGW - 1));
        CY1                   = (int)MinOf(floor((B[3] - PY0) / PolyCell), (double)(PGH - 1));
    };
    for (int p = 0; p < Polys && PGW > 0; p++)
    {
        if (Grid[p].Gx <= 0)
            continue;
        int CX0, CY0, CX1, CY1;
        PlanCells(p, CX0, CY0, CX1, CY1);
        for (int cy = CY0; cy <= CY1; cy++)
            for (int cx = CX0; cx <= CX1; cx++)
            {
                PCount[cy * PGW + cx]++;
                PItems++;
            }
    }
    // The drawn surface's normals at the floor's places (PlaceNormalOf), in the order the records number their places,
    // two words a place - the first half float in the upper half of the first: worked out before the records' room is
    // made, and the lists of each place's triangles let go by then.
    TList<unsigned int> PlaceNormal;
    {
        int RecordPlaces = 0;
        for (int p = 0; p < Polys; p++)
            RecordPlaces += (Grid[p].Gx > 0) ? Grid[p].Places : 0;
        const int  NumTris = R.Tri.Num / 3;
        TList<int> TriPoly, PlaceFirst, PlaceTris, Sign;
        TList<FV3> PolyN;
        if (!PlaceNormal.ReserveExact(2 * RecordPlaces) || !TriPoly.ReserveExact(NumTris) || !TriPoly.Resize(NumTris) ||
            !PlaceFirst.ReserveExact(R.Orig.Num + 1) || !PlaceFirst.Resize(R.Orig.Num + 1) || !Sign.Resize(Polys) ||
            !PolyN.Resize(Polys))
            return false;
        for (int t = 0; t < NumTris; t++)
            TriPoly[t] = -1;
        for (int v = 0; v <= R.Orig.Num; v++)
            PlaceFirst[v] = 0;
        for (int p = 0; p < Polys; p++)
        {
            PolyN[p] = RdxSphere::V3(0.0, 0.0, 0.0);
            Sign[p]  = 1;
            if (p >= R.PolyTris.Num || R.PolyTris[p] <= 0)
                continue;
            FV3 Way = RdxSphere::V3(0.0, 0.0, 0.0);
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                const FV3& A = R.Orig[R.Tri[t * 3]];
                Way          = Way + RdxSphere::Cross(R.Orig[R.Tri[t * 3 + 1]] - A, R.Orig[R.Tri[t * 3 + 2]] - A);
            }
            const FV3 N = (R.PolyPlane.Num >= 4 * Polys)
                              ? RdxSphere::V3(R.PolyPlane[4 * p], R.PolyPlane[4 * p + 1], R.PolyPlane[4 * p + 2])
                              : Way;
            const double L = RdxSphere::Length(N);
            if (!(L > 1e-12))
                continue;
            PolyN[p] = N * (1.0 / L);
            Sign[p]  = (RdxSphere::Dot(Way, PolyN[p]) < 0.0) ? -1 : 1;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                TriPoly[t] = p;
                for (int k = 0; k < 3; k++)
                    PlaceFirst[R.Tri[t * 3 + k] + 1]++;
            }
        }
        for (int v = 0; v < R.Orig.Num; v++)
            PlaceFirst[v + 1] += PlaceFirst[v];
        if (!PlaceTris.ReserveExact(PlaceFirst[R.Orig.Num]) || !PlaceTris.Resize(PlaceFirst[R.Orig.Num]))
            return false;
        // Each place's triangles in their order, its first moved along as they are put in, and put back after.
        for (int t = 0; t < NumTris; t++)
            if (TriPoly[t] >= 0)
                for (int k = 0; k < 3; k++)
                    PlaceTris[PlaceFirst[R.Tri[t * 3 + k]]++] = t;
        for (int v = R.Orig.Num; v > 0; v--)
            PlaceFirst[v] = PlaceFirst[v - 1];
        PlaceFirst[0] = 0;
        for (int p = 0; p < Polys; p++)
        {
            if (Grid[p].Gx <= 0)
                continue;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                for (int k = 0; k < 3; k++)
                {
                    const int v = R.Tri[t * 3 + k];
                    if (Local[v] >= 0)
                        continue;
                    Local[v] = 0;
                    if (!Touched.Add(v))
                        return false;
                    const FV3 N = PlaceNormalOf(R, p, v, PlaceFirst, PlaceTris, TriPoly, PolyN, Sign);
                    PlaceNormal.Add(HalfOf(N.X) << 16);
                    PlaceNormal.Add(HalfOf(N.Y) | (HalfOf(N.Z) << 16));
                }
            for (int i = 0; i < Touched.Num; i++)
                Local[Touched[i]] = -1;
            Touched.Clear();
        }
    }
    const int PolyPiecesAt = PointsAt + RoundPointWords * R.ChordT.Num, RecordsAt = PolyPiecesAt + Polys;
    const int PieceCellsAt = RecordsAt + PieceWords, PieceItemsAt = PieceCellsAt + 2 * PGW * PGH;
    // Its room made at once to its length: grown by doubling, a large level's list would hold up to twice that.
    if (Cells <= 0 || Faces <= 0 || !Out.ReserveExact(PieceItemsAt + PItems) || !Out.Resize(PieceItemsAt + PItems))
        return false;
    auto Bits = [](double V) -> unsigned int
    {
        const float  F = (float)V;
        unsigned int U;
        memcpy(&U, &F, sizeof(U));
        return U;
    };
    Out[0]  = (unsigned int)R.PolyGridW;
    Out[1]  = (unsigned int)R.PolyGridH;
    Out[2]  = Bits(R.PolyGridX0);
    Out[3]  = Bits(R.PolyGridY0);
    Out[4]  = Bits(PolyCell);
    Out[5]  = Bits(R.MoveCap);
    Out[6]  = (unsigned int)CellsAt;
    Out[7]  = (unsigned int)ItemsAt;
    Out[8]  = (unsigned int)PolysAt;
    Out[9]  = (unsigned int)FacesAt;
    Out[10] = (unsigned int)Polys;
    Out[11] = (unsigned int)Faces;
    Out[12] = (unsigned int)ChordsAt;
    Out[13] = (unsigned int)Chords;
    Out[14] = (unsigned int)PointsAt;
    Out[15] = (unsigned int)PolyPiecesAt;
    Out[16] = (unsigned int)PGW;
    Out[17] = (unsigned int)PGH;
    Out[18] = Bits(PX0);
    Out[19] = Bits(PY0);
    Out[20] = (unsigned int)PieceCellsAt;
    Out[21] = (unsigned int)PieceItemsAt;
    for (int c = 0; c < Cells; c++)
    {
        Out[CellsAt + 2 * c]     = (unsigned int)R.PolyGridFirst[c];
        Out[CellsAt + 2 * c + 1] = (unsigned int)R.PolyGridCount[c];
    }
    for (int i = 0; i < R.PolyGridItems.Num; i++)
        Out[ItemsAt + i] = (unsigned int)R.PolyGridItems[i];
    for (int p = 0; p < Polys; p++)
    {
        Out[PolysAt + 2 * p]     = (unsigned int)R.PolyFaceFirst[p];
        Out[PolysAt + 2 * p + 1] = (unsigned int)R.PolyFaceCount[p];
    }
    for (int f = 0; f < Faces; f++)
    {
        unsigned int* const At = &Out[FacesAt + RoundFaceWords * f];
        At[0]                  = (unsigned int)R.FaceKind[f];
        for (int k = 0; k < 4; k++)
        {
            At[1 + k * 3]     = Bits(R.FaceCorner[f * 4 + k].X);
            At[1 + k * 3 + 1] = Bits(R.FaceCorner[f * 4 + k].Y);
            At[1 + k * 3 + 2] = Bits(R.FaceCorner[f * 4 + k].Z);
        }
        for (int k = 0; k < FaceTermFloats; k++)
            At[13 + k] = Bits(R.FaceTerm[f * FaceTermFloats + k]);
    }
    for (int e = 0; e < Chords; e++)
    {
        unsigned int* const At = &Out[ChordsAt + RoundChordWords * e];
        const FV3&          A  = R.ChordEnds[2 * e];
        const FV3&          B  = R.ChordEnds[2 * e + 1];
        At[0]                  = (unsigned int)R.ChordFirst[e];
        At[1]                  = (unsigned int)R.ChordCount[e];
        At[2]                  = Bits(A.X);
        At[3]                  = Bits(A.Y);
        At[4]                  = Bits(A.Z);
        At[5]                  = Bits(B.X);
        At[6]                  = Bits(B.Y);
        At[7]                  = Bits(B.Z);
    }
    for (int i = 0; i < R.ChordT.Num; i++)
    {
        unsigned int* const At = &Out[PointsAt + RoundPointWords * i];
        At[0]                  = Bits(R.ChordT[i]);
        At[1]                  = Bits(R.ChordMove[i].X);
        At[2]                  = Bits(R.ChordMove[i].Y);
        At[3]                  = Bits(R.ChordMove[i].Z);
    }
    // The relief's floor: each polygon's record - its places numbered as they are met, its triangles by them, its
    // grid's cells counted, then filled.
    TList<int> CellFill;
    int        At = RecordsAt, NormalAt = 0;
    for (int p = 0; p < Polys; p++)
    {
        const FPieceGrid& G   = Grid[p];
        Out[PolyPiecesAt + p] = 0;
        if (G.Gx <= 0)
            continue;
        Out[PolyPiecesAt + p] = (unsigned int)At;
        const int First = R.PolyFirst[p], Count = R.PolyTris[p], NumCells = G.Gx * G.Gy;
        const int PlacesOff = RoundPieceHeader + NumCells, TrisOff = PlacesOff + RoundPlaceWords * G.Places;
        const int ItemsOff = TrisOff + 2 * Count;
        unsigned int* const H = &Out[At];
        H[0]                  = Bits(G.N.X);
        H[1]                  = Bits(G.N.Y);
        H[2]                  = Bits(G.N.Z);
        H[3]                  = Bits(G.D);
        H[4]                  = (unsigned int)G.Drop;
        H[5]                  = (unsigned int)G.Gx | ((unsigned int)G.Gy << 16);
        H[6]                  = Bits(G.X0);
        H[7]                  = Bits(G.Y0);
        H[8]                  = Bits(G.InvX);
        H[9]                  = Bits(G.InvY);
        H[10]                 = (unsigned int)PlacesOff;
        H[11]                 = (unsigned int)TrisOff;
        H[12]                 = (unsigned int)ItemsOff;
        int Places = 0;
        for (int t = First; t < First + Count; t++)
        {
            int Idx[3];
            for (int k = 0; k < 3; k++)
            {
                const int v = R.Tri[t * 3 + k];
                if (Local[v] < 0)
                {
                    Local[v] = Places;
                    if (!Touched.Add(v))
                        return false;
                    double A, B;
                    PiecePlanOf(R.Orig[v], G.Drop, A, B);
                    const FV3           M = R.Moved[v] - R.Orig[v];
                    unsigned int* const P = &Out[At + PlacesOff + RoundPlaceWords * Places];
                    P[0]                  = Bits(A);
                    P[1]                  = Bits(B);
                    P[2]                  = HalfOf(M.X) | (HalfOf(M.Y) << 16);
                    P[3]                  = HalfOf(M.Z) | PlaceNormal[NormalAt];
                    P[4]                  = PlaceNormal[NormalAt + 1];
                    NormalAt += 2;
                    Places++;
                }
                Idx[k] = Local[v];
            }
            Out[At + TrisOff + 2 * (t - First)]     = (unsigned int)Idx[0] | ((unsigned int)Idx[1] << 16);
            Out[At + TrisOff + 2 * (t - First) + 1] = (unsigned int)Idx[2];
        }
        for (int i = 0; i < Touched.Num; i++)
            Local[Touched[i]] = -1;
        Touched.Clear();
        if (!CellFill.Resize(NumCells))
            return false;
        for (int c = 0; c < NumCells; c++)
            CellFill[c] = 0;
        for (int t = First; t < First + Count; t++)
        {
            int C0[2], C1[2];
            PieceCellsOf(R, t, G.Drop, G.X0, G.Y0, G.InvX, G.InvY, G.Gx, G.Gy, C0, C1);
            for (int y = C0[1]; y <= C1[1]; y++)
                for (int x = C0[0]; x <= C1[0]; x++)
                    CellFill[y * G.Gx + x]++;
        }
        int Next = 0;
        for (int c = 0; c < NumCells; c++)
        {
            Out[At + RoundPieceHeader + c] = (unsigned int)Next | ((unsigned int)CellFill[c] << 16);
            Next += CellFill[c];
            CellFill[c] = 0;
        }
        for (int i = 0; i < (G.Items + 1) / 2; i++)
            Out[At + ItemsOff + i] = 0;
        for (int t = First; t < First + Count; t++)
        {
            int C0[2], C1[2];
            PieceCellsOf(R, t, G.Drop, G.X0, G.Y0, G.InvX, G.InvY, G.Gx, G.Gy, C0, C1);
            for (int y = C0[1]; y <= C1[1]; y++)
                for (int x = C0[0]; x <= C1[0]; x++)
                {
                    const int c    = y * G.Gx + x;
                    const int Item = (int)(Out[At + RoundPieceHeader + c] & 0xFFFFu) + CellFill[c]++;
                    Out[At + ItemsOff + Item / 2] |= (unsigned int)(t - First) << ((Item & 1) ? 16 : 0);
                }
        }
        At += G.Words;
    }
    int PNext = PieceItemsAt;
    for (int c = 0; c < PGW * PGH; c++)
    {
        Out[PieceCellsAt + 2 * c]     = (unsigned int)PNext;
        Out[PieceCellsAt + 2 * c + 1] = 0;
        PNext += PCount[c];
    }
    for (int p = 0; p < Polys && PGW > 0; p++)
    {
        if (Grid[p].Gx <= 0)
            continue;
        int CX0, CY0, CX1, CY1;
        PlanCells(p, CX0, CY0, CX1, CY1);
        for (int cy = CY0; cy <= CY1; cy++)
            for (int cx = CX0; cx <= CX1; cx++)
            {
                unsigned int* const Cell = &Out[PieceCellsAt + 2 * (cy * PGW + cx)];
                Out[Cell[0] + Cell[1]++] = (unsigned int)p;
            }
    }
    return true;
}

// The card's copy of the relief's outline record E (PackRound) read as SolidPass.hlsl's SphereChordAt reads it: how far
// the place W of the level on that edge moves - as the triangles' side through it moves it, between the two points
// round it in a straight line. False where E is no record, or W lies further than ChordNear off the record's edge: the
// relief's patches were laid by another rounding than the one on the card.
static const double ChordNear = 0.5;
inline bool PackedChordAt(const TList<unsigned int>& Words, unsigned int E, const FV3& W, FV3& Move)
{
    auto F = [&](unsigned int i)
    {
        float V;
        memcpy(&V, &Words[(int)i], sizeof(V));
        return V;
    };
    auto U = [&](unsigned int i) { return Words[(int)i]; };
    Move = RdxSphere::V3(0.0, 0.0, 0.0);
    if (Words.Num < RoundHeader || E >= U(13))
        return false;
    const unsigned int At = U(12) + E * (unsigned int)RoundChordWords, PointsAt = U(14);
    const unsigned int First = U(At), Count = U(At + 1);
    if (Count < 2)
        return false;
    const float Ax = F(At + 2), Ay = F(At + 3), Az = F(At + 4);
    const float Dx = F(At + 5) - Ax, Dy = F(At + 6) - Ay, Dz = F(At + 7) - Az;
    const float Wx = (float)W.X - Ax, Wy = (float)W.Y - Ay, Wz = (float)W.Z - Az;
    const float L2 = Dx * Dx + Dy * Dy + Dz * Dz;
    float       T  = (L2 > 1e-8f) ? (Wx * Dx + Wy * Dy + Wz * Dz) / L2 : 0.0f;
    T              = (T < 0.0f) ? 0.0f : (T > 1.0f) ? 1.0f : T;
    const float Ox = Wx - Dx * T, Oy = Wy - Dy * T, Oz = Wz - Dz * T;
    if (Ox * Ox + Oy * Oy + Oz * Oz > (float)(ChordNear * ChordNear))
        return false;
    unsigned int Lo = 0, Hi = Count - 1;
    while (Hi - Lo > 1)
    {
        const unsigned int Mid = (Lo + Hi) / 2;
        if (F(PointsAt + (First + Mid) * (unsigned int)RoundPointWords) <= T)
            Lo = Mid;
        else
            Hi = Mid;
    }
    const unsigned int P0 = PointsAt + (First + Lo) * (unsigned int)RoundPointWords;
    const unsigned int P1 = PointsAt + (First + Hi) * (unsigned int)RoundPointWords;
    const float        T0 = F(P0), T1 = F(P1);
    float              S  = (T1 > T0) ? (T - T0) / (T1 - T0) : 0.0f;
    S                     = (S < 0.0f) ? 0.0f : (S > 1.0f) ? 1.0f : S;
    Move = RdxSphere::V3(F(P0 + 1) + (F(P1 + 1) - F(P0 + 1)) * S, F(P0 + 2) + (F(P1 + 2) - F(P0 + 2)) * S,
                         F(P0 + 3) + (F(P1 + 3) - F(P0 + 3)) * S);
    return true;
}

// The card's copy (PackRound) read back as SolidPass.hlsl's SphereFaceAt and SphereMoveAt read it, by the plan grid -
// of the place's cell's polygons' faces it lies on, one of the way of its own polygon's plane (Nl, where given) before
// any other and of those the nearest plane, the first found as near as RoundSure at once: where the place W moves, and
// (Found) that polygon. False where it lies on none.
static const double RoundSure = 0.01, RoundAlong = 0.9;
inline bool PackedMoveAt(const TList<unsigned int>& Words, const FV3& W, FV3& Move, int* Found = nullptr,
                         const FV3* Nl = nullptr)
{
    auto F = [&](unsigned int i)
    {
        float V;
        memcpy(&V, &Words[(int)i], sizeof(V));
        return (double)V;
    };
    auto U = [&](unsigned int i) { return Words[(int)i]; };
    if (Words.Num < RoundHeader || U(0) == 0 || U(1) == 0)
        return false;
    const unsigned int GW = U(0), GH = U(1);
    const double       X0 = F(2), Y0 = F(3), Cell = F(4), Cap = F(5);
    const int          Cx = (int)floor((W.X - X0) / Cell), Cy = (int)floor((W.Y - Y0) / Cell);
    if (Cx < 0 || Cy < 0 || Cx >= (int)GW || Cy >= (int)GH)
        return false;
    const unsigned int CellsAt = U(6), ItemsAt = U(7), PolysAt = U(8), FacesAt = U(9);
    const unsigned int C = CellsAt + 2 * ((unsigned int)Cy * GW + (unsigned int)Cx);
    bool               bFound = false, bAlong = false;
    double             Best   = 1e30;
    for (unsigned int i = 0; i < U(C + 1); i++)
    {
        const unsigned int P = U(ItemsAt + U(C) + i);
        for (unsigned int f = U(PolysAt + 2 * P); f < U(PolysAt + 2 * P) + U(PolysAt + 2 * P + 1); f++)
        {
            const unsigned int At = FacesAt + f * (unsigned int)RoundFaceWords;
            FV3                Q[4];
            for (int k = 0; k < 4; k++)
                Q[k] = RdxSphere::V3(F(At + 1 + k * 3), F(At + 2 + k * 3), F(At + 3 + k * 3));
            float  S[3] = { 0.0f, 0.0f, 0.0f };
            double Off  = 0.0;
            FV3    Nf   = RdxSphere::V3(0.0, 0.0, 0.0);
            if (!FaceSharesAt((int)U(At), Q, W, 2.0, S, &Off, &Nf))
                continue;
            const bool bWay = Nl && fabs(RdxSphere::Dot(Nf, *Nl)) >= RoundAlong;
            if (bFound && ((bAlong && !bWay) || (bAlong == bWay && Off >= Best)))
                continue;
            bFound = true;
            bAlong = bWay;
            Best   = Off;
            float Terms[FaceTermFloats];
            for (int k = 0; k < FaceTermFloats; k++)
                Terms[k] = (float)F(At + 13 + k);
            RdxMeshRound::FEdgeTerms T[4];
            FaceTermsOf(Terms, T);
            FaceMoveFrom((int)U(At), T, S, Cap, Move);
            if (Found)
                *Found = (int)P;
            if (bWay && Off <= RoundSure)
                return true;
        }
    }
    return bFound;
}

// The relief's floor on the card (PackRound) read as SolidPass.hlsl's SpherePieceMoveAt reads it, in floats: how far
// the place W of the level moves as the triangle it lies on is drawn. Of the polygons that face the way Nl does
// (RoundAlong) and whose plane it lies within Near of, only those whose plane it lies nearest to, within PiecePlane -
// its own and those in its plane beside it, not the far side of a thin wall: the first triangle of theirs it lies on,
// else the one it lies least outside of if that is within PieceOutside of its shares, its shares held to it. False
// where none is: the faces' move is the relief's there (PackedMoveAt). The polygons looked through: those the floor's own
// plan grid lists in W's cell.
static const float PieceInside = 1e-4f, PieceOutside = 0.05f, PiecePlane = 0.1f;
inline bool PackedPieceMoveAt(const TList<unsigned int>& Words, const FV3& W, const FV3& Nl, FV3& Move,
                              int* Found = nullptr, FV3* Normal = nullptr)
{
    auto F = [&](unsigned int i)
    {
        float V;
        memcpy(&V, &Words[(int)i], sizeof(V));
        return V;
    };
    auto U = [&](unsigned int i) { return Words[(int)i]; };
    Move = RdxSphere::V3(0.0, 0.0, 0.0);
    if (Words.Num < RoundHeader || U(15) == 0 || U(16) == 0 || U(17) == 0)
        return false;
    const unsigned int GW = U(16), GH = U(17);
    const float        X0 = F(18), Y0 = F(19), Cell = F(4);
    const float        Wx = (float)W.X, Wy = (float)W.Y, Wz = (float)W.Z;
    const int          Cx = (int)floorf((Wx - X0) / Cell), Cy = (int)floorf((Wy - Y0) / Cell);
    if (Cx < 0 || Cy < 0 || Cx >= (int)GW || Cy >= (int)GH)
        return false;
    const unsigned int PiecesAt = U(15);
    const unsigned int C        = U(20) + 2 * ((unsigned int)Cy * GW + (unsigned int)Cx);
    // How far W lies off a polygon's plane, where it is one to look at: facing W's way, its plane near.
    auto PlaneOff = [&](unsigned int At)
    {
        const float Nx = F(At), Ny = F(At + 1), Nz = F(At + 2);
        const float Off = fabsf(Nx * Wx + Ny * Wy + Nz * Wz - F(At + 3));
        return (Off > 2.0f || fabsf(Nx * (float)Nl.X + Ny * (float)Nl.Y + Nz * (float)Nl.Z) < (float)RoundAlong) ? 1e30f
                                                                                                               : Off;
    };
    float Nearest = 1e30f;
    for (unsigned int i = 0; i < U(C + 1); i++)
    {
        const unsigned int At = U(PiecesAt + U(U(C) + i));
        if (At != 0)
            Nearest = MinOf(Nearest, PlaneOff(At));
    }
    if (Nearest > 2.0f)
        return false;
    float        Best = -1e30f, BestB[3] = { 0.0f, 0.0f, 0.0f };
    unsigned int BestAt = 0, BestT = 0;
    int          BestP  = -1;
    for (unsigned int i = 0; i < U(C + 1) && Best < -PieceInside; i++)
    {
        const unsigned int P  = U(U(C) + i);
        const unsigned int At = U(PiecesAt + P);
        if (At == 0 || PlaneOff(At) > Nearest + PiecePlane)
            continue;
        const unsigned int Drop = U(At + 4), Gx = U(At + 5) & 0xFFFFu, Gy = U(At + 5) >> 16;
        const unsigned int PlacesAt = At + U(At + 10), TrisAt = At + U(At + 11), ItemsAt = At + U(At + 12);
        const float        A  = (Drop == 0) ? Wy : Wx, B = (Drop == 2) ? Wy : Wz;
        const int          Px = (int)floorf((A - F(At + 6)) * F(At + 8)), Py = (int)floorf((B - F(At + 7)) * F(At + 9));
        const unsigned int Cw = U(At + (unsigned int)RoundPieceHeader +
                                  (unsigned int)((Py < 0) ? 0 : (Py >= (int)Gy) ? (int)Gy - 1 : Py) * Gx +
                                  (unsigned int)((Px < 0) ? 0 : (Px >= (int)Gx) ? (int)Gx - 1 : Px));
        for (unsigned int j = 0; j < (Cw >> 16) && Best < -PieceInside; j++)
        {
            const unsigned int Item = (Cw & 0xFFFFu) + j;
            const unsigned int T    = (U(ItemsAt + Item / 2) >> ((Item & 1) ? 16 : 0)) & 0xFFFFu;
            const unsigned int I01 = U(TrisAt + 2 * T), I2 = U(TrisAt + 2 * T + 1);
            const unsigned int I[3] = { I01 & 0xFFFFu, I01 >> 16, I2 };
            float              Pa[3], Pb[3];
            for (int k = 0; k < 3; k++)
            {
                Pa[k] = F(PlacesAt + (unsigned int)RoundPlaceWords * I[k]);
                Pb[k] = F(PlacesAt + (unsigned int)RoundPlaceWords * I[k] + 1);
            }
            const float E1a = Pa[1] - Pa[0], E1b = Pb[1] - Pb[0], E2a = Pa[2] - Pa[0], E2b = Pb[2] - Pb[0];
            const float Den = E1a * E2b - E2a * E1b;
            if (!(fabsf(Den) > 1e-12f))
                continue;
            const float Qa = A - Pa[0], Qb = B - Pb[0];
            const float B1 = (Qa * E2b - E2a * Qb) / Den, B2 = (E1a * Qb - Qa * E1b) / Den, B0 = 1.0f - B1 - B2;
            const float L  = MinOf(B0, MinOf(B1, B2));
            if (L > Best)
            {
                Best     = L;
                BestB[0] = B0;
                BestB[1] = B1;
                BestB[2] = B2;
                BestAt   = At;
                BestT    = T;
                BestP    = (int)P;
            }
        }
    }
    if (BestP < 0 || Best < -PieceOutside)
        return false;
    float S[3], Sum = 0.0f;
    for (int k = 0; k < 3; k++)
    {
        S[k] = MaxOf(BestB[k], 0.0f);
        Sum += S[k];
    }
    const unsigned int PlacesAt = BestAt + U(BestAt + 10), TrisAt = BestAt + U(BestAt + 11);
    const unsigned int I01 = U(TrisAt + 2 * BestT), I2 = U(TrisAt + 2 * BestT + 1);
    const unsigned int I[3] = { I01 & 0xFFFFu, I01 >> 16, I2 };
    float              M[3] = { 0.0f, 0.0f, 0.0f }, N[3] = { 0.0f, 0.0f, 0.0f };
    for (int k = 0; k < 3; k++)
    {
        const unsigned int V = PlacesAt + (unsigned int)RoundPlaceWords * I[k];
        M[0] += FloatOfHalf(U(V + 2) & 0xFFFFu) * (S[k] / Sum);
        M[1] += FloatOfHalf(U(V + 2) >> 16) * (S[k] / Sum);
        M[2] += FloatOfHalf(U(V + 3) & 0xFFFFu) * (S[k] / Sum);
        N[0] += FloatOfHalf(U(V + 3) >> 16) * S[k];
        N[1] += FloatOfHalf(U(V + 4) & 0xFFFFu) * S[k];
        N[2] += FloatOfHalf(U(V + 4) >> 16) * S[k];
    }
    Move = RdxSphere::V3(M[0], M[1], M[2]);
    if (Normal)
    {
        // As the card has it: the corners' normals between them, of length one, turned to Nl's side.
        const float L = sqrtf(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
        *Normal       = (L > 1e-6f) ? RdxSphere::V3(N[0] / L, N[1] / L, N[2] / L) : Nl;
        if (RdxSphere::Dot(*Normal, Nl) < 0.0)
            *Normal = *Normal * -1.0;
    }
    if (Found)
        *Found = BestP;
    return true;
}

// The moved polygon a point of the level's space lies on, within Near of it - by the plan grid, the first found; -1 none
// (PolygonMoveAt: and the point's move with it, MoveOnPolygon's).
inline int PolygonMoveAt(const FResult& R, const FV3& P, double Near, FV3& Move)
{
    if (R.PolyGridW <= 0 || R.PolyGridH <= 0)
        return -1;
    const int CX = (int)floor((P.X - R.PolyGridX0) / PolyCell), CY = (int)floor((P.Y - R.PolyGridY0) / PolyCell);
    if (CX < 0 || CY < 0 || CX >= R.PolyGridW || CY >= R.PolyGridH)
        return -1;
    const int c = CY * R.PolyGridW + CX;
    for (int i = 0; i < R.PolyGridCount[c]; i++)
    {
        const int p = R.PolyGridItems[R.PolyGridFirst[c] + i];
        if (MoveOnPolygon(R, p, P, Near, Move))
            return p;
    }
    return -1;
}

inline int PolygonAt(const FResult& R, const FV3& P, double Near)
{
    FV3 Move;
    return PolygonMoveAt(R, P, Near, Move);
}

}   // namespace RdxLevelRound

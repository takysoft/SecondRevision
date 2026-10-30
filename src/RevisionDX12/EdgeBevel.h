// SecondRevision - a hard edge's fine bevel (docs/terv-latvany-roadmap.md 9.3): where flat faces of the level meet at
// hard outward edges, the solid is rounded there by a ball of Radius - as if it were worn down by the radius and grown
// back by it (an opening): a box's edges become quarter cylinders and its corners eighths of a ball, and the faces
// away from the edges stay where they are. The edge then takes the light along a thin line, as a real one does, and
// its outline is rounded too. The polygons are to be cut along the bands first; each point is then moved here.
//
// - An edge (FEdge, MakeEdge) is a whole straight run of hard edge between the same two planes, not one piece of it the
//   BSP cut: its line from one end to the other, its two faces' outward normals, and the ways from the line into each
//   face. Only an outward edge is one - its faces turn away from each other, as a box's do - and only one that turns
//   by more than the least and less than the most the caller allows: a nearly flat one has nothing to round, and a
//   knife's edge is left sharp.
// - A point is in an edge's band where it lies on one of the edge's faces (within OnFace of its plane), no further in
//   from the line than the band's width - the radius times the tangent of half the turn: a right angle's band is as
//   wide as the radius -, and beside the run (no further past its ends than that width).
// - The planes of the faces of every edge whose band holds the point, each pushed in by the radius, bound the solid
//   worn down there. Its nearest place to the point (Inner) is found among the planes one, two and three at a time;
//   the point goes onto the ball of the radius round it, and its normal points from it. A point of a face no edge's
//   band holds, or one whose nearest worn place lies straight under it, stays.
// - A point takes only the edges whose faces it lies on, so a wall rising beside a ledge does not follow the ledge's
//   round; and the point alone decides where it goes, whichever face it is taken from, so the faces still meet.
// - Where the solid is thinner than twice the radius - a frame's thin side - there is no worn solid left: Round says
//   so, and leaves the point.
//
// Engine-independent, as Spherify.h: the unit tests run the same code.
#pragma once

#include <math.h>

#include "Spherify.h"

namespace RdxBevel
{

using RdxSphere::Cross;
using RdxSphere::Dot;
using RdxSphere::FV3;
using RdxSphere::Length;
using RdxSphere::V3;

static const double OnFace   = 0.01;   // units: a point this near a face's plane lies on it
static const int    MostPlanes = 8;    // the planes that may bound the worn solid round one point

struct FEdge
{
    FV3    From;           // the line's first end
    FV3    Way;            // along the line, unit
    double Length;         // from the first end to the last
    FV3    NormalA, NormalB;
    FV3    IntoA, IntoB;   // from the line into each face, unit, square to the line
    double Turn;           // radians between the two normals
};

inline FV3 Unit(FV3 A)
{
    const double L = Length(A);
    return (L > 0.0) ? A * (1.0 / L) : A;
}

// The edge from P0 to P1 between the faces of outward normals NormalA and NormalB, InA a point of face A off the line
// (which side of the line the face lies on): false where it is no outward edge turning by more than LeastTurn and less
// than MostTurn radians.
inline bool MakeEdge(FV3 P0, FV3 P1, FV3 NormalA, FV3 NormalB, FV3 InA, double LeastTurn, double MostTurn, FEdge& E)
{
    const FV3    Line = P1 - P0;
    const double L    = Length(Line);
    if (L <= 0.0)
        return false;
    E.From    = P0;
    E.Way     = Line * (1.0 / L);
    E.Length  = L;
    E.NormalA = Unit(NormalA);
    E.NormalB = Unit(NormalB);
    E.Turn    = acos(fmax(-1.0, fmin(1.0, Dot(E.NormalA, E.NormalB))));
    if (!(E.Turn > LeastTurn && E.Turn < MostTurn))
        return false;
    // Into A: square to the line within A's plane, towards InA. Into B the same within B's plane, the way that goes
    // below A's plane - the way an outward edge's B goes; an inner edge's A then goes above B's plane, and is refused.
    E.IntoA = Unit(Cross(E.NormalA, E.Way));
    if (Dot(E.IntoA, InA - P0) < 0.0)
        E.IntoA = E.IntoA * -1.0;
    E.IntoB = Unit(Cross(E.NormalB, E.Way));
    if (Dot(E.IntoB, E.NormalA) > 0.0)
        E.IntoB = E.IntoB * -1.0;
    return Dot(E.IntoA, E.NormalB) < 0.0 && Dot(E.IntoB, E.NormalA) < 0.0;
}

// How far into each face the edge's band reaches, for a ball of Radius.
inline double BandWidth(const FEdge& E, double Radius) { return Radius * tan(0.5 * E.Turn); }

// Whether P lies in the edge's band.
inline bool InBand(const FEdge& E, FV3 P, double Radius)
{
    const FV3    Off   = P - E.From;
    const double Band  = BandWidth(E, Radius);
    const double Along = Dot(Off, E.Way);
    if (Along < -Band || Along > E.Length + Band)
        return false;
    const double InA = Dot(Off, E.IntoA), InB = Dot(Off, E.IntoB);
    if (fabs(Dot(Off, E.NormalA)) <= OnFace && InA > -OnFace && InA < Band)
        return true;
    return fabs(Dot(Off, E.NormalB)) <= OnFace && InB > -OnFace && InB < Band;
}

namespace Detail
{

// The nearest place to P on the planes Pick names (one, two or three of N, X . Normal = Level), or false where they
// do not meet in one.
inline bool OnPlanes(FV3 P, const FV3* Normal, const double* Level, const int* Pick, int Picks, FV3& At)
{
    if (Picks == 1)
    {
        At = P - Normal[Pick[0]] * (Dot(P, Normal[Pick[0]]) - Level[Pick[0]]);
        return true;
    }
    if (Picks == 2)
    {
        const FV3    N0 = Normal[Pick[0]], N1 = Normal[Pick[1]];
        const double A = Dot(N0, N0), B = Dot(N0, N1), C = Dot(N1, N1);
        const double Det = A * C - B * B;
        if (fabs(Det) < 1e-12)
            return false;
        const double R0 = Dot(P, N0) - Level[Pick[0]], R1 = Dot(P, N1) - Level[Pick[1]];
        const double S0 = (C * R0 - B * R1) / Det, S1 = (A * R1 - B * R0) / Det;
        At = P - N0 * S0 - N1 * S1;
        return true;
    }
    const FV3    N0 = Normal[Pick[0]], N1 = Normal[Pick[1]], N2 = Normal[Pick[2]];
    const FV3    C12 = Cross(N1, N2), C20 = Cross(N2, N0), C01 = Cross(N0, N1);
    const double Det = Dot(N0, C12);
    if (fabs(Det) < 1e-12)
        return false;
    At = (C12 * Level[Pick[0]] + C20 * Level[Pick[1]] + C01 * Level[Pick[2]]) * (1.0 / Det);
    return true;
}

} // namespace Detail

// Where the edges' rounds of Radius take P: the moved place and its normal there. How many planes bound the worn solid
// round P - 0 where no edge's band holds P (Moved is P, Normal untouched); -1 where the solid is too thin there to wear
// down by the radius (Moved is P).
inline int Round(const FEdge* Edges, int Count, FV3 P, double Radius, FV3& Moved, FV3& Normal)
{
    Moved = P;
    FV3    Planes[MostPlanes];
    double Levels[MostPlanes];
    int    N = 0;
    for (int i = 0; i < Count; i++)
    {
        if (!InBand(Edges[i], P, Radius))
            continue;
        const FV3 Faces[2] = { Edges[i].NormalA, Edges[i].NormalB };
        for (int f = 0; f < 2; f++)
        {
            const double Level = Dot(Faces[f], Edges[i].From) - Radius;
            bool bKnown = false;
            for (int k = 0; k < N && !bKnown; k++)
                bKnown = Length(Planes[k] - Faces[f]) < 1e-9 && fabs(Levels[k] - Level) < 1e-6;
            if (!bKnown && N < MostPlanes)
            {
                Planes[N] = Faces[f];
                Levels[N] = Level;
                N++;
            }
        }
    }
    if (N == 0)
        return 0;
    // The worn solid's nearest place: of the places on one, two or three of the planes, the nearest that lies within
    // them all.
    double Best  = 1e300;
    FV3    Inner = P;
    auto   Try   = [&](const int* Pick, int Picks) {
        FV3 At;
        if (!Detail::OnPlanes(P, Planes, Levels, Pick, Picks, At))
            return;
        for (int k = 0; k < N; k++)
            if (Dot(At, Planes[k]) > Levels[k] + 1e-7)
                return;
        const double D = Length(At - P);
        if (D < Best)
        {
            Best  = D;
            Inner = At;
        }
    };
    for (int a = 0; a < N; a++)
    {
        const int One[1] = { a };
        Try(One, 1);
        for (int b = a + 1; b < N; b++)
        {
            const int Two[2] = { a, b };
            Try(Two, 2);
            for (int c = b + 1; c < N; c++)
            {
                const int Three[3] = { a, b, c };
                Try(Three, 3);
            }
        }
    }
    if (Best >= 1e300)
        return -1;
    if (Best <= Radius * (1.0 + 1e-9))
        return N;
    Normal = (P - Inner) * (1.0 / Best);
    Moved  = Inner + Normal * Radius;
    return N;
}

static const double OnLine = 1e-4;   // units: a corner this near a cutting line lies on it

// A convex face cut along the bands of edges of it, for Round to move the band's points: Q its N corners in order,
// Normal its outward normal. For each edge, the lines in the face's plane parallel to the edge's line at the band's width
// (BandWidth) and at half of it - two slices across the round - cut every piece they cross. Points: the face's corners
// first, then each new point once - a point a line puts on a side two pieces share is the same for both, so they meet
// without a crack; Pieces: each piece's count of corners, then its corners in order, as indices into Points. The band
// goes into the face from the edge: the edge's IntoA where the face is its A, IntoB where it is its B. False where there
// was no memory.
inline bool CutBands(const FV3* Q, int N, FV3 Normal, const FEdge* Edges, int Count, double Radius,
                     RdxSphere::TList<FV3>& Points, RdxSphere::TList<int>& Pieces)
{
    Points.Clear();
    Pieces.Clear();
    if (N < 3)
        return true;
    if (!Pieces.Add(N))
        return false;
    for (int k = 0; k < N; k++)
        if (!Points.Add(Q[k]) || !Pieces.Add(k))
            return false;
    // The points lines put on sides: the side's two ends (the lower first) and the line, for the point once made.
    RdxSphere::TList<long long> Made;
    RdxSphere::TList<int>       MadeAt;
    RdxSphere::TList<int>       Next, Under, Over;
    int                         Line = 0;
    for (int e = 0; e < Count; e++)
    {
        const FEdge& E    = Edges[e];
        const FV3    Into = (Dot(E.NormalA, Normal) >= Dot(E.NormalB, Normal)) ? E.IntoA : E.IntoB;
        const double Band = BandWidth(E, Radius);
        for (int s = 0; s < 2; s++, Line++)
        {
            const double At = (s == 0) ? Band : 0.5 * Band;
            auto Side = [&](int i) {
                const double D = Dot(Points[i] - E.From, Into) - At;
                return (D > OnLine) ? 1 : ((D < -OnLine) ? -1 : 0);
            };
            Next.Clear();
            for (int p = 0; p < Pieces.Num;)
            {
                const int  n = Pieces[p];
                const int* C = &Pieces[p + 1];
                bool       bUnder = false, bOver = false;
                for (int k = 0; k < n; k++)
                {
                    const int S = Side(C[k]);
                    bUnder      = bUnder || S < 0;
                    bOver       = bOver || S > 0;
                }
                Under.Clear();
                Over.Clear();
                if (!(bUnder && bOver))
                {
                    if (!Next.Add(n))
                        return false;
                    for (int k = 0; k < n; k++)
                        if (!Next.Add(C[k]))
                            return false;
                    p += n + 1;
                    continue;
                }
                for (int k = 0; k < n; k++)
                {
                    const int i = C[k], j = C[(k + 1) % n];
                    const int Si = Side(i), Sj = Side(j);
                    if ((Si <= 0 && !Under.Add(i)) || (Si >= 0 && !Over.Add(i)))
                        return false;
                    if (Si * Sj >= 0)
                        continue;
                    // The side crosses the line: its point there, made once for both pieces that share the side.
                    const long long Lo = (i < j) ? i : j, Hi = (i < j) ? j : i;
                    const long long Key = (Lo << 40) | (Hi << 16) | (long long)Line;
                    int             Point = -1;
                    for (int m = 0; m < Made.Num && Point < 0; m++)
                        if (Made[m] == Key)
                            Point = MadeAt[m];
                    if (Point < 0)
                    {
                        const double Di = Dot(Points[i] - E.From, Into) - At, Dj = Dot(Points[j] - E.From, Into) - At;
                        const FV3    X  = Points[i] + (Points[j] - Points[i]) * (Di / (Di - Dj));
                        Point           = Points.Num;
                        if (!Points.Add(X) || !Made.Add(Key) || !MadeAt.Add(Point))
                            return false;
                    }
                    if (!Under.Add(Point) || !Over.Add(Point))
                        return false;
                }
                const RdxSphere::TList<int>* const Parts[2] = { &Under, &Over };
                for (int q = 0; q < 2; q++)
                {
                    const RdxSphere::TList<int>& Part = *Parts[q];
                    if (Part.Num < 3)
                        continue;
                    if (!Next.Add(Part.Num))
                        return false;
                    for (int k = 0; k < Part.Num; k++)
                        if (!Next.Add(Part[k]))
                            return false;
                }
                p += n + 1;
            }
            if (!Pieces.Resize(Next.Num))
                return false;
            for (int k = 0; k < Next.Num; k++)
                Pieces[k] = Next[k];
        }
    }
    return true;
}

} // namespace RdxBevel

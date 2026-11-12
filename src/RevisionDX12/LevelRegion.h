// SecondRevision - the level's rounding a cube at a time (LevelRound.h's Round, LevelBevel.h's Bevel), so that the whole
// level is never held at once: which of the level's polygons a cube's work takes for its own polygons to come out
// exactly as the whole level's would.
//
// A flat polygon (polygons sharing sides in one plane) is rounded as one piece (LevelRound.h's FCoarse): the move of its
// curved border carries across the whole of it, so a cube takes every flat polygon of its own polygons whole; the
// polygons sharing a place with those, and their flat polygons whole; the polygons sharing a place with all of those -
// the faces about their places, whose normals their edges bend by; every polygon with a side on the line of a hard side
// of one taken (the fine bevel's edges are runs of hard sides along a line, each bevelled or left sharp as a whole);
// every polygon whose box comes within RegionNear of
// one taken - the still ones a taken place's move may be held behind (LevelRound.h's HoldBehind), and those the bevel's
// bands about an edge reach -, and the lines of the hard sides of those again; and every polygon whose box comes within
// RegionRound of the cube. tools\LevelRoundCheck's region mode holds a cube's polygons so made against
// the whole level's.
//
// Engine-independent, as LevelRound.h: the renderer and tools\LevelRoundCheck run the same code.
#pragma once

#include "LevelRound.h"

namespace RdxLevelRound
{

static const double RegionNear  = 20.0;   // units: past the most a place moves (the cap's 8) and the fine bevel's
                                          // reach about an edge
static const double RegionRound = 64.0;   // units: the round about the cube itself
static const double RegionGrid  = 64.0;   // units: the cells the polygons' boxes are found by
static const double RegionCoarse = 1024.0; // units: and the cells of a box over more than RegionSpan of those
static const int    RegionSpan  = 64;     // a box over more cells than this goes to the coarser cells, and over more
                                          // of those, it is looked at for every question

// The level's polygons as a cube's work asks for them: made once a level (RegionIndexMake).
struct FRegionIndex
{
    int                NumPolys = 0;
    TList<int>         FlatOf;              // a polygon: its flat polygon, by the lowest of its polygons
    TList<int>         FlatStart, FlatList; // a flat polygon by its lowest polygon: its polygons (FlatStart: NumPolys + 1)
    Detail::FCells     PolysAt;             // a place, to the 64th of a unit: the polygons with a corner there
    TList<FV3>         Lo, Hi;              // a polygon's box
    Detail::FCells     Grid;                // a cell of RegionGrid: the polygons whose box reaches into it
    Detail::FCells     Coarse;              // a cell of RegionCoarse: the polygons too large for Grid
    TList<int>         Big;                 // the polygons too large for Coarse as well
    // Each side by its lower place's key (SideOf) and its other's (SideKey, by SideOf's entry), its polygon; each line a
    // side lies on (RunLine), the polygons with a side on it; and the cosine of the soft turn - a side across which two
    // polygons turn further is hard.
    Detail::FCells     SideOf;
    TList<long long>   SideKey;
    Detail::FCells     OnLine;
    double             SoftCos = 0.0;

    void Free()
    {
        FlatOf.Free();
        FlatStart.Free();
        FlatList.Free();
        PolysAt.Free();
        Lo.Free();
        Hi.Free();
        Grid.Free();
        Coarse.Free();
        Big.Free();
        SideOf.Free();
        SideKey.Free();
        OnLine.Free();
        NumPolys = 0;
    }
};

namespace RegionDetail
{
inline long long PlaceKey(const FV3& X)
{
    return Detail::CellKey((long long)floor(X.X * 64.0 + 0.5), (long long)floor(X.Y * 64.0 + 0.5),
                           (long long)floor(X.Z * 64.0 + 0.5));
}
inline long long GridOf(double V) { return (long long)floor(V / RegionGrid); }
inline long long CoarseOf(double V) { return (long long)floor(V / RegionCoarse); }
// The line through A and B: its way (one way round of the two) to a 512th, and its point nearest the origin to a quarter
// of a unit, as one number.
inline long long RunLine(const FV3& A, const FV3& B)
{
    FV3          D = B - A;
    const double L = RdxSphere::Length(D);
    if (!(L > 1e-6))
        return 0;
    D = D * (1.0 / L);
    if (D.X < -1e-9 || (fabs(D.X) <= 1e-9 && (D.Y < -1e-9 || (fabs(D.Y) <= 1e-9 && D.Z < 0.0))))
        D = D * -1.0;
    const FV3       P  = A - D * RdxSphere::Dot(A, D);
    const long long H1 = Detail::CellKey((long long)floor(D.X * 512.0 + 0.5), (long long)floor(D.Y * 512.0 + 0.5),
                                         (long long)floor(D.Z * 512.0 + 0.5));
    const long long H2 = Detail::CellKey((long long)floor(P.X * 4.0 + 0.5), (long long)floor(P.Y * 4.0 + 0.5),
                                         (long long)floor(P.Z * 4.0 + 0.5));
    return (long long)((unsigned long long)H1 * 0x9E3779B97F4A7C15ull ^ (unsigned long long)H2);
}
inline bool      Near(const FV3& ALo, const FV3& AHi, const FV3& BLo, const FV3& BHi, double D)
{
    return ALo.X <= BHi.X + D && AHi.X >= BLo.X - D && ALo.Y <= BHi.Y + D && AHi.Y >= BLo.Y - D && ALo.Z <= BHi.Z + D &&
           AHi.Z >= BLo.Z - D;
}
} // namespace RegionDetail

// The index of Polys (corners in Points), the spherifier's Angle: the flat polygons, the places' polygons, the sides and
// their lines, the boxes. False: no memory.
inline bool RegionIndexMake(const FV3* Points, int NumPoints, const FPolyIn* Polys, int NumPolys, double Angle,
                            FRegionIndex& I)
{
    using namespace RegionDetail;
    I.Free();
    I.NumPolys = NumPolys;
    I.SoftCos  = cos((180.0 - Angle) * 3.14159265358979 / 180.0);
    if (!I.FlatOf.Resize(NumPolys) || !I.Lo.Resize(NumPolys) || !I.Hi.Resize(NumPolys))
        return false;
    int Corners = 0;
    for (int q = 0; q < NumPolys; q++)
        Corners += (Polys[q].Count > 0) ? Polys[q].Count : 0;
    // The boxes, the places' polygons, the sides' and the lines'.
    Detail::FCells&   SideOf  = I.SideOf;
    TList<long long>& SideKey = I.SideKey;
    if (!I.PolysAt.Init(Corners) || !SideOf.Init(Corners) || !I.OnLine.Init(Corners))
        return false;
    for (int q = 0; q < NumPolys; q++)
    {
        const FPolyIn& P = Polys[q];
        FV3            L = RdxSphere::V3(0.0, 0.0, 0.0), H = L;
        for (int k = 0; k < P.Count; k++)
        {
            const int Ia = P.First + k, Ib = P.First + (k + 1) % P.Count;
            if (Ia < 0 || Ia >= NumPoints || Ib < 0 || Ib >= NumPoints)
                continue;
            const FV3& X = Points[Ia];
            if (k == 0)
                L = H = X;
            L = RdxSphere::V3(MinOf(L.X, X.X), MinOf(L.Y, X.Y), MinOf(L.Z, X.Z));
            H = RdxSphere::V3(MaxOf(H.X, X.X), MaxOf(H.Y, X.Y), MaxOf(H.Z, X.Z));
            const long long A = PlaceKey(X), B = PlaceKey(Points[Ib]);
            const long long Lo2 = (A < B) ? A : B, Hi2 = (A < B) ? B : A;
            if (!I.PolysAt.Add(A, q) || !SideOf.Add(Lo2, q) || !SideKey.Add(Hi2) ||
                !I.OnLine.Add(RunLine(X, Points[Ib]), q))
                return false;
        }
        I.Lo[q] = L;
        I.Hi[q] = H;
    }
    // The flat polygons: polygons sharing a side in one plane, joined.
    TList<int> Up;
    if (!Up.Resize(NumPolys))
        return false;
    for (int q = 0; q < NumPolys; q++)
        Up[q] = q;
    auto Root = [&](int q) {
        while (Up[q] != q)
            q = Up[q] = Up[Up[q]];
        return q;
    };
    for (int e = 0; e < SideOf.Key.Num; e++)
    {
        const FPolyIn& A = Polys[SideOf.Item[e]];
        for (int g = SideOf.Next[e]; g >= 0; g = SideOf.Next[g])
        {
            if (SideOf.Key[g] != SideOf.Key[e] || SideKey[g] != SideKey[e])
                continue;
            const FPolyIn& B = Polys[SideOf.Item[g]];
            if (A.Nx * B.Nx + A.Ny * B.Ny + A.Nz * B.Nz >= 0.99996 && fabs(A.W - B.W) <= 0.01)
            {
                const int Ra = Root(SideOf.Item[e]), Rb = Root(SideOf.Item[g]);
                Up[Ra] = Rb;
            }
        }
    }
    // By the lowest polygon of each: the members, in rising order.
    TList<int> Low;
    if (!Low.Resize(NumPolys) || !I.FlatStart.Resize(NumPolys + 1) || !I.FlatList.Resize(NumPolys))
        return false;
    for (int q = 0; q < NumPolys; q++)
        Low[q] = -1;
    for (int q = 0; q < NumPolys; q++)
    {
        const int r = Root(q);
        if (Low[r] < 0)
            Low[r] = q;
        I.FlatOf[q] = Low[r];
    }
    for (int q = 0; q <= NumPolys; q++)
        I.FlatStart[q] = 0;
    for (int q = 0; q < NumPolys; q++)
        I.FlatStart[I.FlatOf[q] + 1]++;
    for (int q = 0; q < NumPolys; q++)
        I.FlatStart[q + 1] += I.FlatStart[q];
    TList<int> Fill;
    if (!Fill.Resize(NumPolys))
        return false;
    for (int q = 0; q < NumPolys; q++)
        Fill[q] = I.FlatStart[q];
    for (int q = 0; q < NumPolys; q++)
        I.FlatList[Fill[I.FlatOf[q]]++] = q;
    // The boxes by the grid's cells - a box over too many of them by the coarser cells, and one over too many of those
    // looked at always.
    if (!I.Grid.Init(NumPolys * 4) || !I.Coarse.Init(1024))
        return false;
    for (int q = 0; q < NumPolys; q++)
    {
        const long long X0 = GridOf(I.Lo[q].X), X1 = GridOf(I.Hi[q].X), Y0 = GridOf(I.Lo[q].Y), Y1 = GridOf(I.Hi[q].Y);
        const long long Z0 = GridOf(I.Lo[q].Z), Z1 = GridOf(I.Hi[q].Z);
        if ((X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1) <= RegionSpan)
        {
            for (long long x = X0; x <= X1; x++)
                for (long long y = Y0; y <= Y1; y++)
                    for (long long z = Z0; z <= Z1; z++)
                        if (!I.Grid.Add(Detail::CellKey(x, y, z), q))
                            return false;
            continue;
        }
        const long long U0 = CoarseOf(I.Lo[q].X), U1 = CoarseOf(I.Hi[q].X), V0 = CoarseOf(I.Lo[q].Y);
        const long long V1 = CoarseOf(I.Hi[q].Y), W0 = CoarseOf(I.Lo[q].Z), W1 = CoarseOf(I.Hi[q].Z);
        if ((U1 - U0 + 1) * (V1 - V0 + 1) * (W1 - W0 + 1) > RegionSpan)
        {
            if (!I.Big.Add(q))
                return false;
            continue;
        }
        for (long long x = U0; x <= U1; x++)
            for (long long y = V0; y <= V1; y++)
                for (long long z = W0; z <= W1; z++)
                    if (!I.Coarse.Add(Detail::CellKey(x, y, z), q))
                        return false;
    }
    return true;
}

// Each polygon whose box comes within D of the box Lo..Hi, once (Seen, a stamp a polygon: Stamp marks it), into Out.
inline bool RegionBoxNear(const FRegionIndex& I, const FV3& Lo, const FV3& Hi, double D, TList<int>& Seen, int Stamp,
                          TList<int>& Out)
{
    using namespace RegionDetail;
    auto Try = [&](int q) {
        if (Seen[q] == Stamp || !Near(I.Lo[q], I.Hi[q], Lo, Hi, D))
            return true;
        Seen[q] = Stamp;
        return Out.Add(q);
    };
    const long long X0 = GridOf(Lo.X - D), X1 = GridOf(Hi.X + D), Y0 = GridOf(Lo.Y - D), Y1 = GridOf(Hi.Y + D);
    const long long Z0 = GridOf(Lo.Z - D), Z1 = GridOf(Hi.Z + D);
    for (long long x = X0; x <= X1; x++)
        for (long long y = Y0; y <= Y1; y++)
            for (long long z = Z0; z <= Z1; z++)
            {
                const long long K = Detail::CellKey(x, y, z);
                for (int e = I.Grid.Head[I.Grid.Bucket(K)]; e >= 0; e = I.Grid.Next[e])
                    if (I.Grid.Key[e] == K && !Try(I.Grid.Item[e]))
                        return false;
            }
    const long long U0 = CoarseOf(Lo.X - D), U1 = CoarseOf(Hi.X + D), V0 = CoarseOf(Lo.Y - D), V1 = CoarseOf(Hi.Y + D);
    const long long W0 = CoarseOf(Lo.Z - D), W1 = CoarseOf(Hi.Z + D);
    for (long long x = U0; x <= U1; x++)
        for (long long y = V0; y <= V1; y++)
            for (long long z = W0; z <= W1; z++)
            {
                const long long K = Detail::CellKey(x, y, z);
                for (int e = I.Coarse.Head[I.Coarse.Bucket(K)]; e >= 0; e = I.Coarse.Next[e])
                    if (I.Coarse.Key[e] == K && !Try(I.Coarse.Item[e]))
                        return false;
            }
    for (int b = 0; b < I.Big.Num; b++)
        if (!Try(I.Big[b]))
            return false;
    return true;
}

// The polygons the work of the cube from CubeLo, Size a side, takes - its own (the middle of its corners in the cube)
// and their round, in rising order - into Out, and its own into Own. False: no memory.
inline bool RegionPolysOf(const FRegionIndex& I, const FV3* Points, const FPolyIn* Polys, const FV3& CubeLo, double Size,
                          TList<int>& Own, TList<int>& Out)
{
    using namespace RegionDetail;
    const int            N = I.NumPolys;
    TList<unsigned char> In;
    TList<int>           Seen, More;
    if (!In.Resize(N) || !Seen.Resize(N))
        return false;
    for (int q = 0; q < N; q++)
    {
        In[q]   = 0;
        Seen[q] = 0;
    }
    Own.Clear();
    Out.Clear();
    const FV3 CubeHi = CubeLo + RdxSphere::V3(Size, Size, Size);
    for (int q = 0; q < N; q++)
    {
        const FPolyIn& P = Polys[q];
        if (P.Count <= 0)
            continue;
        FV3 Mid = RdxSphere::V3(0.0, 0.0, 0.0);
        for (int k = 0; k < P.Count; k++)
            Mid = Mid + Points[P.First + k] * (1.0 / P.Count);
        if (floor(Mid.X / Size) * Size == CubeLo.X && floor(Mid.Y / Size) * Size == CubeLo.Y &&
            floor(Mid.Z / Size) * Size == CubeLo.Z && !Own.Add(q))
            return false;
    }
    // A polygon's flat polygon, whole.
    auto TakeFlat = [&](int q) {
        const int f = I.FlatOf[q];
        for (int i = I.FlatStart[f]; i < I.FlatStart[f + 1]; i++)
            In[I.FlatList[i]] = 1;
    };
    // The polygons sharing a place with one taken now, into More.
    auto Ring = [&]() {
        More.Clear();
        for (int q = 0; q < N; q++)
        {
            if (!In[q])
                continue;
            const FPolyIn& P = Polys[q];
            for (int k = 0; k < P.Count; k++)
            {
                const long long K = PlaceKey(Points[P.First + k]);
                for (int e = I.PolysAt.Head[I.PolysAt.Bucket(K)]; e >= 0; e = I.PolysAt.Next[e])
                    if (I.PolysAt.Key[e] == K && !More.Add(I.PolysAt.Item[e]))
                        return false;
            }
        }
        return true;
    };
    for (int i = 0; i < Own.Num; i++)
        TakeFlat(Own[i]);
    for (int i = 0; i < Own.Num; i++)
        In[Own[i]] = 1;
    if (!Ring())
        return false;
    for (int i = 0; i < More.Num; i++)
        TakeFlat(More[i]);
    if (!Ring())
        return false;
    for (int i = 0; i < More.Num; i++)
        In[More[i]] = 1;
    // The polygons on the lines of the hard sides of those taken.
    auto Runs = [&]() {
        More.Clear();
        for (int q = 0; q < N; q++)
        {
            if (!In[q])
                continue;
            const FPolyIn& P = Polys[q];
            for (int k = 0; k < P.Count; k++)
            {
                const FV3&      A = Points[P.First + k];
                const FV3&      B = Points[P.First + (k + 1) % P.Count];
                const long long Ka = PlaceKey(A), Kb = PlaceKey(B);
                const long long Lo2 = (Ka < Kb) ? Ka : Kb, Hi2 = (Ka < Kb) ? Kb : Ka;
                bool            bHard = false;
                for (int e = I.SideOf.Head[I.SideOf.Bucket(Lo2)]; e >= 0 && !bHard; e = I.SideOf.Next[e])
                {
                    const int m = I.SideOf.Item[e];
                    bHard = I.SideOf.Key[e] == Lo2 && I.SideKey[e] == Hi2 && m != q &&
                            Polys[m].Nx * P.Nx + Polys[m].Ny * P.Ny + Polys[m].Nz * P.Nz < I.SoftCos;
                }
                if (!bHard)
                    continue;
                const long long Ln = RunLine(A, B);
                for (int e = I.OnLine.Head[I.OnLine.Bucket(Ln)]; e >= 0; e = I.OnLine.Next[e])
                    if (I.OnLine.Key[e] == Ln && !More.Add(I.OnLine.Item[e]))
                        return false;
            }
        }
        for (int i = 0; i < More.Num; i++)
            In[More[i]] = 1;
        return true;
    };
    // Each polygon whose box comes within RegionNear of one taken now - or, bCube, within RegionRound of the cube - taken.
    auto TakeNear = [&](bool bCube) {
        More.Clear();
        for (int q = 0; q < N; q++)
            if (In[q])
                Seen[q] = 1;
        if (bCube)
        {
            if (!RegionBoxNear(I, CubeLo, CubeHi, RegionRound, Seen, 1, More))
                return false;
        }
        else
            for (int q = 0; q < N; q++)
                if (In[q] && !RegionBoxNear(I, I.Lo[q], I.Hi[q], RegionNear, Seen, 1, More))
                    return false;
        for (int i = 0; i < More.Num; i++)
            In[More[i]] = 1;
        return true;
    };
    if (!Runs() || !TakeNear(false) || !Runs() || !TakeNear(true))
        return false;
    for (int q = 0; q < N; q++)
        if (In[q] && !Out.Add(q))
            return false;
    return true;
}

} // namespace RdxLevelRound

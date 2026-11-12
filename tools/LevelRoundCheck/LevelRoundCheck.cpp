// LevelRoundCheck: the renderer's rounding of the level (src\RevisionDX12\LevelRound.h) run on a level's polygons written
// out by scripts\level_round_check.py, with no game and no graphics card - and what must hold of what it makes checked:
//
// - no place moves further than the cap;
// - nothing opens: every edge of the moved polygons' triangles is used by two of them or more, or else lies where
//   nothing moved, on the straight edge the engine draws an unmoved neighbour with - or the level's own open edge;
// - no triangle is turned over against its polygon or flat;
// - the light sides (the light maps' blending) lie along their polygons' own sides, their runs inside them;
// - the card's copy (PackRound) moves the polygons' places as the CPU does;
// - and the relief's floor on the card (PackedPieceMoveAt) puts a place where the drawn triangle it lies on has it,
//   and lifts it along a normal near that triangle's own.
//
//     LevelRoundCheck <in> [angle, degrees]
//
// With LRC_RELIEF=1 in the environment every wall is taken as one the relief may be drawn on (Spherify.h's Use_Relief),
// whose edges are cut close to their curve all along - the most cutting the rounding does. With LRC_FLOOR_NEED, how
// many of the floor's triangles stand off the faces' curve, and how many whole polygons would be had at a few bounds.
//
//     LevelRoundCheck <in> [angle] bevel                 the fine bevel too (LevelBevel.h), and its checks
//     LevelRoundCheck <in> [angle] region <cell> <halo>  the level a cube at a time, each cube's polygons held against
//                                                        the whole level's (LRC_REGIONS cubes, the densest first)
//
// The region mode's switches: LRC_REGION_INDEX=1 the cubes' polygons by LevelRegion.h (the renderer's rule);
// LRC_REGION_BEVEL=1 the fine bevel on the whole level and on each cube; LRC_REGION_ALONE=1 the cubes only, for the
// most memory one takes; LRC_REGION_SAY=1 each polygon off, with its flat polygon. The experiments the rule came from:
// LRC_HALO_FLAT=1 (flat polygons whole, two rings), LRC_HALO_RING2=1, LRC_HALO_NEAR=<units>, LRC_HALO_RUNS=1..4 (the
// hard sides' lines: 1 whole lines, 2 again after the near ones, 3 connected runs, 4 the bevel's own chain),
// LRC_HALO_SOFT=1, LRC_HALO_FLATMAX=<units>. LRC_MEMLAPS=1: the memory at each part of the rounding and the bevel.
//
// LRC_RBYTES=1: what the result holds after the bevel, list by list. LRC_COMPACT=1: the places kept in single precision
// at the end, as the renderer keeps them (FResult::Compact), and what that changes of the card's copy and the decals'
// moves. LRC_VARIANT_CUBE=<units>: the level rounded once more with the wide flat polygons' pieces kept within cubes
// (FResult::PieceCube), and its drawn surface set against the one drawn (LRC_VARIANT_TOP=<n>: the n polygons apart the
// most; LRC_VARIANT_ONLY=1: nothing after it). LRC_TILES=<n>: the level laid n by n, a level that many times as large.
// LRC_ROOM=<MB>: the rounding and the bevel given that much memory, a block past it refused (RoomWithin).
// LRC_PLACES=<w>,...: those places where they were and went, with their polygons. LRC_SEAMS=1: the still walls' sides
// a moved place lay on and left (a seam the still wall does not follow), the widest open ones said.
// LRC_RENDER="x,y,z,yaw,pitch,fov,w,h,prefix": a picture of what the rounding draws from that eye (and of each variant's),
// <prefix>-<what>.ppm.
//
// In (little-endian): "SPI1", the cap (double), the places (int count, then three doubles each), the polygons (int
// count, then each one's first place, count, plane's four doubles and use). One line to the console: what was made,
// what failed, and how long it took.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Spherify.h"

// The rounding's folds as UnfoldSurface finds them (LEVELROUND_TRACE): with LRC_TRACE_BOX=x0,y0,z0,x1,y1,z1 in the
// environment, each piece or quad found folding with a corner in that box, and the round it was found in.
static double GTraceBox[6] = { 1e30, 1e30, 1e30, -1e30, -1e30, -1e30 };
inline void LevelRoundTraceFold(int Round, const RdxSphere::FV3& A, const RdxSphere::FV3& B, const RdxSphere::FV3& C)
{
    const RdxSphere::FV3 P[3] = { A, B, C };
    bool                 bIn  = false;
    for (int k = 0; k < 3 && !bIn; k++)
        bIn = P[k].X >= GTraceBox[0] && P[k].Y >= GTraceBox[1] && P[k].Z >= GTraceBox[2] && P[k].X <= GTraceBox[3] &&
              P[k].Y <= GTraceBox[4] && P[k].Z <= GTraceBox[5];
    if (bIn)
        printf("  fold: round %d, (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)\n", Round, A.X, A.Y, A.Z, B.X, B.Y,
               B.Z, C.X, C.Y, C.Z);
}
// And each true edge's terms as MeshRound.h works them out, for an edge with an end in that box: its kind, ends, the
// ways its ends leave along (one: a way, zero: straight), its terms, cap and rooms.
inline void LevelRoundTraceEdge(int Kind, float AX, float AY, float AZ, float BX, float BY, float BZ, float WayA, float WayB,
                                float TA, float TB, float Cap, float RA, float RB)
{
    const float P[2][3] = { { AX, AY, AZ }, { BX, BY, BZ } };
    bool        bIn     = false;
    for (int k = 0; k < 2 && !bIn; k++)
        bIn = P[k][0] >= GTraceBox[0] && P[k][1] >= GTraceBox[1] && P[k][2] >= GTraceBox[2] && P[k][0] <= GTraceBox[3] &&
              P[k][1] <= GTraceBox[4] && P[k][2] <= GTraceBox[5];
    if (bIn)
        printf("  edge: kind %d, (%.2f %.2f %.2f)-(%.2f %.2f %.2f), ways %.0f %.0f, terms %.3f %.3f, cap %.3f, rooms %.3f "
               "%.3f\n", Kind, AX, AY, AZ, BX, BY, BZ, WayA, WayB, TA, TB, Cap, RA, RB);
}
// And each edge's kind and how many faces it has, as the shape sorts them - the shape's number among those made (Shape:
// the edge with the lowest number of each, so a new shape is told by its first edge coming again).
static int GTraceShape = 0, GTraceLastEdge = -1;
inline void LevelRoundTraceEdgeKind(int E, int Kind, int Faces, float AX, float AY, float AZ, float BX, float BY, float BZ)
{
    if (E <= GTraceLastEdge)
        GTraceShape++;
    GTraceLastEdge = E;
    const float P[2][3] = { { AX, AY, AZ }, { BX, BY, BZ } };
    bool        bIn     = false;
    for (int k = 0; k < 2 && !bIn; k++)
        bIn = P[k][0] >= GTraceBox[0] && P[k][1] >= GTraceBox[1] && P[k][2] >= GTraceBox[2] && P[k][0] <= GTraceBox[3] &&
              P[k][1] <= GTraceBox[4] && P[k][2] <= GTraceBox[5];
    if (bIn)
        printf("  kind: shape %d, %d, %d faces, (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", GTraceShape, Kind, Faces, AX, AY, AZ,
               BX, BY, BZ);
}
// The sheets the rounding holds where the surface would move them off their plane (RoundOnce's step 5), for the
// summary - and with LRC_TRACE_HELD in the environment each one's polygon.
static int GSheetsHeld = 0;
inline void LevelRoundTraceSheetHeld(int Poly)
{
    GSheetsHeld++;
    if (getenv("LRC_TRACE_HELD"))
        printf("  held: sheet %d\n", Poly);
}
#define LEVELROUND_TRACE 1
#include "LevelRound.h"

#include "EdgeBevel.h"
#define LEVELBEVEL_TRACE 1
#include "LevelBevel.h"
#include "LevelRegion.h"

#include <dbghelp.h>
#include <mmsystem.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

// The rounding's result as one number (FNV-1a over everything it hands back, field by field): two builds whose numbers
// agree on a level made the same rounding of it, to the bit.
static unsigned long long HashBytes(unsigned long long H, const void* Data, size_t Bytes)
{
    const unsigned char* P = (const unsigned char*)Data;
    for (size_t i = 0; i < Bytes; i++)
    {
        H ^= P[i];
        H *= 1099511628211ull;
    }
    return H;
}
template <class T> static unsigned long long HashList(unsigned long long H, const RdxSphere::TList<T>& L)
{
    H = HashBytes(H, &L.Num, sizeof(L.Num));
    return HashBytes(H, L.Data, sizeof(T) * (size_t)L.Num);
}
static unsigned long long ResultHash(const RdxSphere::FResult& R)
{
    unsigned long long H = 1469598103934665603ull;
    H = HashList(H, R.Orig);
    H = HashList(H, R.Moved);
    H = HashList(H, R.PolyFirst);
    H = HashList(H, R.PolyTris);
    H = HashList(H, R.Tri);
    H = HashList(H, R.TriTurn);
    H = HashList(H, R.PolyFaceFirst);
    H = HashList(H, R.PolyFaceCount);
    H = HashList(H, R.FaceKind);
    H = HashList(H, R.FaceCorner);
    H = HashList(H, R.FaceTerm);
    H = HashList(H, R.PolyGridFirst);
    H = HashList(H, R.PolyGridCount);
    H = HashList(H, R.PolyGridItems);
    H = HashList(H, R.Left);
    H = HashList(H, R.PolyEdgeKinds);
    H = HashList(H, R.PolySideFirst);
    H = HashList(H, R.PolySideCount);
    for (int i = 0; i < R.LightSides.Num; i++)
    {
        const RdxSphere::FLightSide& S = R.LightSides[i];
        H = HashBytes(H, &S.A, sizeof(S.A));
        H = HashBytes(H, &S.B, sizeof(S.B));
        H = HashBytes(H, &S.Into, sizeof(S.Into));
        H = HashBytes(H, &S.PartFirst, sizeof(S.PartFirst));
        H = HashBytes(H, &S.PartCount, sizeof(S.PartCount));
    }
    for (int i = 0; i < R.LightParts.Num; i++)
    {
        const RdxSphere::FLightPart& P = R.LightParts[i];
        H = HashBytes(H, &P.T0, sizeof(P.T0));
        H = HashBytes(H, &P.T1, sizeof(P.T1));
        H = HashBytes(H, &P.Other, sizeof(P.Other));
        H = HashBytes(H, &P.Into, sizeof(P.Into));
        H = HashBytes(H, &P.Depth, sizeof(P.Depth));
        H = HashBytes(H, &P.Reach, sizeof(P.Reach));
    }
    H = HashList(H, R.PolyChordFirst);
    H = HashList(H, R.ChordFirst);
    H = HashList(H, R.ChordCount);
    H = HashList(H, R.ChordEnds);
    H = HashList(H, R.ChordT);
    H = HashList(H, R.ChordMove);
    const int Counts[] = { R.PolyGridW, R.PolyGridH, R.ChordPolys, R.ChordMissed, R.Walls, R.Segments, R.SoftEdges,
                           R.Creases, R.TPoints, R.Folded, R.Touched, R.Through, R.Stuck, R.Pieces, R.Quads, R.Unbent,
                           R.Zipped };
    const double Numbers[] = { R.MoveCap, R.PolyGridX0, R.PolyGridY0, R.LargestMove };
    H = HashBytes(H, Counts, sizeof(Counts));
    return HashBytes(H, Numbers, sizeof(Numbers));
}

// A polygon's triangles in a result, laid flat along the largest axis of its normal where their places were (Orig),
// for a point of its plane to be found on one: by a grid of about one cell a triangle.
struct FFlatTris
{
    int              First = 0, Count = 0, Drop = 2;
    double           X0 = 0.0, Y0 = 0.0, Cw = 1.0, Ch = 1.0;
    int              Gw = 1, Gh = 1;
    std::vector<int> CellFirst, Items;

    void Flat(const RdxSphere::FV3& P, double& X, double& Y) const
    {
        X = (Drop == 0) ? P.Y : P.X;
        Y = (Drop == 2) ? P.Y : P.Z;
    }
    void Make(const RdxSphere::FResult& R, int p, const RdxSphere::FV3& N)
    {
        First = R.PolyFirst[p];
        Count = R.PolyTris[p];
        const double Ax = fabs(N.X), Ay = fabs(N.Y), Az = fabs(N.Z);
        Drop            = (Ax >= Ay && Ax >= Az) ? 0 : ((Ay >= Az) ? 1 : 2);
        double Lx = 1e300, Ly = 1e300, Hx = -1e300, Hy = -1e300;
        for (int t = First; t < First + Count; t++)
            for (int k = 0; k < 3; k++)
            {
                double X, Y;
                Flat(R.OrigAt(R.Tri[t * 3 + k]), X, Y);
                Lx = (X < Lx) ? X : Lx;
                Ly = (Y < Ly) ? Y : Ly;
                Hx = (X > Hx) ? X : Hx;
                Hy = (Y > Hy) ? Y : Hy;
            }
        const int Side = (int)ceil(sqrt((double)Count));
        Gw = Gh = (Side < 1) ? 1 : ((Side > 256) ? 256 : Side);
        X0 = Lx;
        Y0 = Ly;
        Cw = (Hx - Lx > 1e-9) ? (Hx - Lx) / Gw : 1.0;
        Ch = (Hy - Ly > 1e-9) ? (Hy - Ly) / Gh : 1.0;
        std::vector<int> Counts((size_t)Gw * Gh + 1, 0);
        auto Cells = [&](int t, int& Ax0, int& Ay0, int& Ax1, int& Ay1) {
            double Lo[2] = { 1e300, 1e300 }, Hi[2] = { -1e300, -1e300 };
            for (int k = 0; k < 3; k++)
            {
                double X, Y;
                Flat(R.OrigAt(R.Tri[t * 3 + k]), X, Y);
                Lo[0] = (X < Lo[0]) ? X : Lo[0];
                Lo[1] = (Y < Lo[1]) ? Y : Lo[1];
                Hi[0] = (X > Hi[0]) ? X : Hi[0];
                Hi[1] = (Y > Hi[1]) ? Y : Hi[1];
            }
            Ax0 = CellX(Lo[0] - 1e-6);
            Ay0 = CellY(Lo[1] - 1e-6);
            Ax1 = CellX(Hi[0] + 1e-6);
            Ay1 = CellY(Hi[1] + 1e-6);
        };
        for (int t = First; t < First + Count; t++)
        {
            int Ax0, Ay0, Ax1, Ay1;
            Cells(t, Ax0, Ay0, Ax1, Ay1);
            for (int y = Ay0; y <= Ay1; y++)
                for (int x = Ax0; x <= Ax1; x++)
                    Counts[(size_t)y * Gw + x]++;
        }
        CellFirst.assign((size_t)Gw * Gh + 1, 0);
        for (int c = 0; c < Gw * Gh; c++)
            CellFirst[c + 1] = CellFirst[c] + Counts[c];
        Items.assign((size_t)CellFirst[(size_t)Gw * Gh], 0);
        std::vector<int> At(CellFirst.begin(), CellFirst.end() - 1);
        for (int t = First; t < First + Count; t++)
        {
            int Ax0, Ay0, Ax1, Ay1;
            Cells(t, Ax0, Ay0, Ax1, Ay1);
            for (int y = Ay0; y <= Ay1; y++)
                for (int x = Ax0; x <= Ax1; x++)
                    Items[At[(size_t)y * Gw + x]++] = t;
        }
    }
    int CellX(double X) const
    {
        const int C = (int)floor((X - X0) / Cw);
        return (C < 0) ? 0 : ((C >= Gw) ? Gw - 1 : C);
    }
    int CellY(double Y) const
    {
        const int C = (int)floor((Y - Y0) / Ch);
        return (C < 0) ? 0 : ((C >= Gh) ? Gh - 1 : C);
    }
    // Where R draws P: on the triangle P lies most inside of where it was, by its shares of their corners there, moved
    // as they are; false where it lies in none within a hair.
    bool DrawnAt(const RdxSphere::FResult& R, const RdxSphere::FV3& P, RdxSphere::FV3& Out) const
    {
        double X, Y;
        Flat(P, X, Y);
        const int c    = CellY(Y) * Gw + CellX(X);
        double    Best = -1e300;
        for (int i = CellFirst[c]; i < CellFirst[c + 1]; i++)
        {
            const int  t = Items[i];
            const int  a = R.Tri[t * 3], b = R.Tri[t * 3 + 1], d = R.Tri[t * 3 + 2];
            double     Xa, Ya, Xb, Yb, Xd, Yd;
            Flat(R.OrigAt(a), Xa, Ya);
            Flat(R.OrigAt(b), Xb, Yb);
            Flat(R.OrigAt(d), Xd, Yd);
            const double Den = (Xb - Xa) * (Yd - Ya) - (Xd - Xa) * (Yb - Ya);
            if (!(fabs(Den) > 1e-12))
                continue;
            const double B1  = ((X - Xa) * (Yd - Ya) - (Xd - Xa) * (Y - Ya)) / Den;
            const double B2  = ((Xb - Xa) * (Y - Ya) - (X - Xa) * (Yb - Ya)) / Den;
            const double B0  = 1.0 - B1 - B2;
            const double Low = (B0 < B1) ? ((B0 < B2) ? B0 : B2) : ((B1 < B2) ? B1 : B2);
            if (Low <= Best)
                continue;
            Best = Low;
            Out  = R.MovedAt(a) * B0 + R.MovedAt(b) * B1 + R.MovedAt(d) * B2;
        }
        return Best >= -1e-4;
    }
};

// A picture of what a result draws (LRC_RENDER): every wall - a moved one as its triangles where they went, any other as
// the level has it - from an eye at Cam[0..2] looking along Cam[3] (yaw, degrees about the upright) and Cam[4] (pitch),
// Cam[5] degrees wide, W by H pixels, each triangle grey by how it faces a light from over the eye's shoulder - a moved
// polygon's a little blue -, cut at a unit before the eye; written as a binary PPM to Out.
static void RenderResult(const RdxSphere::FResult& R, const RdxSphere::FV3* Pts, const RdxSphere::FPolyIn* Polys,
                         int NumPolys, const double* Cam, int W, int H, const char* Out)
{
    using RdxSphere::FV3;
    const double Pi = 3.14159265358979323846;
    const double Yaw = Cam[3] * Pi / 180.0, Pitch = Cam[4] * Pi / 180.0;
    const FV3    Eye = RdxSphere::V3(Cam[0], Cam[1], Cam[2]);
    const FV3    F   = RdxSphere::V3(cos(Pitch) * cos(Yaw), cos(Pitch) * sin(Yaw), sin(Pitch));
    const FV3    Rt  = RdxSphere::V3(-sin(Yaw), cos(Yaw), 0.0);
    const FV3    Up  = RdxSphere::Cross(F, Rt);
    const double Foc = (W * 0.5) / tan(Cam[5] * Pi / 360.0);
    FV3          Lt  = F * -1.0 + Up * 0.6 + Rt * 0.4;
    Lt               = Lt * (1.0 / RdxSphere::Length(Lt));
    std::vector<float>         Depth((size_t)W * H, 0.0f);   // one over the eye's depth: the nearer the larger
    std::vector<unsigned char> Rgb((size_t)W * H * 3, 0);
    for (size_t i = 0; i < Rgb.size(); i += 3)
    {
        Rgb[i]     = 30;
        Rgb[i + 1] = 30;
        Rgb[i + 2] = 40;
    }
    auto Tri = [&](FV3 A, FV3 B, FV3 C, bool bMoved) {
        const FV3    Nn = RdxSphere::Cross(B - A, C - A);
        const double Ln = RdxSphere::Length(Nn);
        if (!(Ln > 0.0))
            return;
        const double Shade = 0.15 + 0.85 * fabs(RdxSphere::Dot(Nn * (1.0 / Ln), Lt));
        // Into the eye's space, cut at a unit before the eye.
        double Q[8][3];
        int    Nq = 0;
        double In[3][3];
        const FV3 Cs[3] = { A, B, C };
        for (int k = 0; k < 3; k++)
        {
            const FV3 D = Cs[k] - Eye;
            In[k][0]    = RdxSphere::Dot(D, Rt);
            In[k][1]    = RdxSphere::Dot(D, Up);
            In[k][2]    = RdxSphere::Dot(D, F);
        }
        for (int k = 0; k < 3; k++)
        {
            const double* P0 = In[k];
            const double* P1 = In[(k + 1) % 3];
            const bool    b0 = P0[2] >= 1.0, b1 = P1[2] >= 1.0;
            if (b0)
            {
                Q[Nq][0] = P0[0];
                Q[Nq][1] = P0[1];
                Q[Nq][2] = P0[2];
                Nq++;
            }
            if (b0 != b1)
            {
                const double T = (1.0 - P0[2]) / (P1[2] - P0[2]);
                for (int j = 0; j < 3; j++)
                    Q[Nq][j] = P0[j] + (P1[j] - P0[j]) * T;
                Nq++;
            }
        }
        if (Nq < 3)
            return;
        double Sx[8], Sy[8], Iz[8];
        for (int k = 0; k < Nq; k++)
        {
            Sx[k] = W * 0.5 + Q[k][0] / Q[k][2] * Foc;
            Sy[k] = H * 0.5 - Q[k][1] / Q[k][2] * Foc;
            Iz[k] = 1.0 / Q[k][2];
        }
        const unsigned char Rc = (unsigned char)(bMoved ? 200 * Shade : 225 * Shade);
        const unsigned char Gc = (unsigned char)(bMoved ? 215 * Shade : 225 * Shade);
        const unsigned char Bc = (unsigned char)(bMoved ? 255 * Shade : 225 * Shade);
        for (int k = 1; k + 1 < Nq; k++)
        {
            const int    I[3] = { 0, k, k + 1 };
            const double X0 = Sx[I[0]], Y0 = Sy[I[0]], X1 = Sx[I[1]], Y1 = Sy[I[1]], X2 = Sx[I[2]], Y2 = Sy[I[2]];
            const double Area = (X1 - X0) * (Y2 - Y0) - (X2 - X0) * (Y1 - Y0);
            if (fabs(Area) < 1e-12)
                continue;
            using RdxSphere::MaxOf;
            using RdxSphere::MinOf;
            const int Lx = (int)MaxOf(0.0, floor(MinOf(X0, MinOf(X1, X2))));
            const int Hx = (int)MinOf((double)W - 1, ceil(MaxOf(X0, MaxOf(X1, X2))));
            const int Ly = (int)MaxOf(0.0, floor(MinOf(Y0, MinOf(Y1, Y2))));
            const int Hy = (int)MinOf((double)H - 1, ceil(MaxOf(Y0, MaxOf(Y1, Y2))));
            for (int y = Ly; y <= Hy; y++)
                for (int x = Lx; x <= Hx; x++)
                {
                    const double Px = x + 0.5, Py = y + 0.5;
                    const double W0 = ((X1 - Px) * (Y2 - Py) - (X2 - Px) * (Y1 - Py)) / Area;
                    const double W1 = ((X2 - Px) * (Y0 - Py) - (X0 - Px) * (Y2 - Py)) / Area;
                    const double W2 = 1.0 - W0 - W1;
                    if (W0 < 0.0 || W1 < 0.0 || W2 < 0.0)
                        continue;
                    const float Z = (float)(W0 * Iz[I[0]] + W1 * Iz[I[1]] + W2 * Iz[I[2]]);
                    const size_t At = (size_t)y * W + x;
                    if (Z <= Depth[At])
                        continue;
                    Depth[At]       = Z;
                    Rgb[At * 3]     = Rc;
                    Rgb[At * 3 + 1] = Gc;
                    Rgb[At * 3 + 2] = Bc;
                }
        }
    };
    for (int p = 0; p < NumPolys; p++)
    {
        const RdxSphere::FPolyIn& P = Polys[p];
        if (!(P.Use & RdxSphere::Use_Wall) || P.Count < 3)
            continue;
        if (p < R.PolyTris.Num && R.PolyTris[p] > 0)
        {
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                Tri(R.MovedAt(R.Tri[t * 3]), R.MovedAt(R.Tri[t * 3 + 1]), R.MovedAt(R.Tri[t * 3 + 2]), true);
            continue;
        }
        for (int k = 1; k + 1 < P.Count; k++)
            Tri(Pts[P.First], Pts[P.First + k], Pts[P.First + k + 1], false);
    }
    if (FILE* O = fopen(Out, "wb"))
    {
        fprintf(O, "P6\n%d %d\n255\n", W, H);
        fwrite(Rgb.data(), 1, Rgb.size(), O);
        fclose(O);
        printf("render: %s\n", Out);
    }
}
// LRC_RENDER="x,y,z,yaw,pitch,fov,w,h,prefix": the camera and the pictures' names (RenderResult) - <prefix>-<what>.ppm.
static bool RenderAsked(double* Cam, int& W, int& H, char* Prefix, int PrefixRoom)
{
    const char* const T = getenv("LRC_RENDER");
    if (!T)
        return false;
    char Name[512] = {};
    if (sscanf(T, "%lf,%lf,%lf,%lf,%lf,%lf,%d,%d,%511s", &Cam[0], &Cam[1], &Cam[2], &Cam[3], &Cam[4], &Cam[5], &W, &H,
               Name) != 9)
        return false;
    snprintf(Prefix, PrefixRoom, "%s", Name);
    return W > 0 && H > 0;
}

// Two results of one level set against each other where they draw it: each polygon's places of the one's triangles
// where the other draws them - a place the other's triangles of the polygon have too, where it was (Orig, to the bit),
// as that one is moved (the nearest of them, where it has more than one); any other found on those triangles; and a
// polygon left as the engine draws it there drawing them where they were -, how far apart the two draw them, and of
// that how much along the polygon's normal (off its plane, what shows), at the most and past half a millimetre, a
// tenth, a unit; where it is most; and the places found on no triangle of the other's. A place on no shared one is
// found on the triangle it lies most inside of, which is not its own where the triangles overlap where they were.
static void SurfacesApart(const RdxSphere::FResult& A, const RdxSphere::FResult& B, const RdxSphere::FV3* Pts,
                          const RdxSphere::FPolyIn* Polys, int NumPolys, const char* What)
{
    std::vector<std::pair<double, int>> PolyUp;   // each polygon's most off its plane, past half a mm
    double     Most = 0.0, MostUp = 0.0;
    long long  Set = 0, Missed = 0, PastMm = 0, PastTenth = 0, PastUnit = 0, UpPastMm = 0, UpPastTenth = 0;
    int        WorstPoly = -1;
    RdxSphere::FV3 WorstAt = RdxSphere::V3(0.0, 0.0, 0.0);
    std::vector<int> PolysPastMm;
    for (int p = 0; p < NumPolys; p++)
    {
        const bool bA = p < A.PolyTris.Num && A.PolyTris[p] > 0, bB = p < B.PolyTris.Num && B.PolyTris[p] > 0;
        if (!bA && !bB)
            continue;
        const RdxSphere::FV3 N = RdxSphere::V3(Polys[p].Nx, Polys[p].Ny, Polys[p].Nz);
        FFlatTris            Fa, Fb;
        if (bA)
            Fa.Make(A, p, N);
        if (bB)
            Fb.Make(B, p, N);
        bool   bPastMm = false;
        double PolyMost = 0.0;
        for (int Side = 0; Side < 2; Side++)
        {
            const RdxSphere::FResult&   From = Side ? B : A;
            const RdxSphere::FResult&   To   = Side ? A : B;
            const FFlatTris& On   = Side ? Fa : Fb;
            const bool       bOn  = Side ? bA : bB;
            if (!(Side ? bB : bA))
                continue;
            // The other's places of the polygon by where they were.
            std::map<std::tuple<double, double, double>, std::vector<int>> Shared;
            if (bOn)
                for (int t = To.PolyFirst[p]; t < To.PolyFirst[p] + To.PolyTris[p]; t++)
                    for (int k = 0; k < 3; k++)
                    {
                        const int            w = To.Tri[t * 3 + k];
                        const RdxSphere::FV3 O = To.OrigAt(w);
                        std::vector<int>&    L = Shared[std::make_tuple(O.X, O.Y, O.Z)];
                        if (std::find(L.begin(), L.end(), w) == L.end())
                            L.push_back(w);
                    }
            for (int t = From.PolyFirst[p]; t < From.PolyFirst[p] + From.PolyTris[p]; t++)
                for (int k = 0; k < 3; k++)
                {
                    const int            v = From.Tri[t * 3 + k];
                    const RdxSphere::FV3 P = From.OrigAt(v);
                    RdxSphere::FV3       There = P;
                    const auto           Same = bOn ? Shared.find(std::make_tuple(P.X, P.Y, P.Z)) : Shared.end();
                    if (Same != Shared.end())
                    {
                        double Near = 1e300;
                        for (const int w : Same->second)
                        {
                            const double Dw = RdxSphere::Length(From.MovedAt(v) - To.MovedAt(w));
                            if (Dw < Near)
                            {
                                Near  = Dw;
                                There = To.MovedAt(w);
                            }
                        }
                    }
                    else if (bOn && !On.DrawnAt(To, P, There))
                    {
                        Missed++;
                        continue;
                    }
                    const RdxSphere::FV3 D  = From.MovedAt(v) - There;
                    const double         L  = RdxSphere::Length(D);
                    const double         Up = fabs(RdxSphere::Dot(D, N));
                    Set++;
                    PastMm += (L > 0.025) ? 1 : 0;
                    PastTenth += (L > 0.1) ? 1 : 0;
                    PastUnit += (L > 1.0) ? 1 : 0;
                    UpPastMm += (Up > 0.025) ? 1 : 0;
                    UpPastTenth += (Up > 0.1) ? 1 : 0;
                    bPastMm  = bPastMm || Up > 0.025;
                    PolyMost = (Up > PolyMost) ? Up : PolyMost;
                    if (L > Most)
                    {
                        Most      = L;
                        WorstPoly = p;
                        WorstAt   = P;
                    }
                    MostUp = (Up > MostUp) ? Up : MostUp;
                }
        }
        if (bPastMm)
        {
            PolysPastMm.push_back(p);
            PolyUp.push_back(std::make_pair(PolyMost, p));
        }
    }
    printf("%s: places set %lld (found on no triangle %lld); apart at the most %.4f, off the plane %.4f; past half a mm "
           "%lld (off the plane %lld), past a tenth %lld (off the plane %lld), past a unit %lld; polygons off the plane "
           "past half a mm %d\n",
           What, Set, Missed, Most, MostUp, PastMm, UpPastMm, PastTenth, UpPastTenth, PastUnit, (int)PolysPastMm.size());
    if (WorstPoly >= 0)
        printf("%s: the most at %.2f %.2f %.2f, polygon %d\n", What, WorstAt.X, WorstAt.Y, WorstAt.Z, WorstPoly);
    // LRC_VARIANT_TOP=<n>: the n polygons off their planes the most - how far, how wide each is (its box's widest
    // side), its normal and its middle.
    const int Top = getenv("LRC_VARIANT_TOP") ? atoi(getenv("LRC_VARIANT_TOP")) : 0;
    std::sort(PolyUp.begin(), PolyUp.end(), [](const std::pair<double, int>& X, const std::pair<double, int>& Y) {
        return X.first > Y.first;
    });
    for (int i = 0; i < Top && i < (int)PolyUp.size(); i++)
    {
        const RdxSphere::FPolyIn& P  = Polys[PolyUp[i].second];
        RdxSphere::FV3            Lo = Pts[P.First], Hi = Lo, Mid = RdxSphere::V3(0.0, 0.0, 0.0);
        for (int k = 0; k < P.Count; k++)
        {
            const RdxSphere::FV3& Q = Pts[P.First + k];
            Lo  = RdxSphere::V3((Q.X < Lo.X) ? Q.X : Lo.X, (Q.Y < Lo.Y) ? Q.Y : Lo.Y, (Q.Z < Lo.Z) ? Q.Z : Lo.Z);
            Hi  = RdxSphere::V3((Q.X > Hi.X) ? Q.X : Hi.X, (Q.Y > Hi.Y) ? Q.Y : Hi.Y, (Q.Z > Hi.Z) ? Q.Z : Hi.Z);
            Mid = Mid + Q * (1.0 / P.Count);
        }
        const RdxSphere::FV3 E = Hi - Lo;
        const double         W = (E.X > E.Y) ? ((E.X > E.Z) ? E.X : E.Z) : ((E.Y > E.Z) ? E.Y : E.Z);
        printf("%s: polygon %d off its plane by %.3f; %.0f wide, normal %.2f %.2f %.2f, middle %.0f %.0f %.0f\n", What,
               PolyUp[i].second, PolyUp[i].first, W, P.Nx, P.Ny, P.Nz, Mid.X, Mid.Y, Mid.Z);
    }
}

// The sampler reads the 32-bit thread context: a 64-bit build of the tool (for the largest levels) goes without it.
#if !defined(_WIN64)
// With LRC_SAMPLE=<rounds> in the environment: where the rounding spends its time. It is made that many times more
// before the one checked, the main thread stopped every millisecond meanwhile and its place noted; then the places by
// function - the one the code was inlined into, and the innermost inlined one - and by line, by the tool's own symbols.
static HANDLE        GSampled     = nullptr;
static HANDLE        GSampleQuit  = nullptr;
static DWORD*        GSamples     = nullptr;
static DWORD*        GCallers     = nullptr;   // a place outside the tool: the nearest return address into it, or 0
static volatile LONG GSampleCount = 0;
static const LONG    SamplesMost  = 1 << 22;
static DWORD         GImageLo = 0, GImageHi = 0;
static DWORD         GStackLo = 0, GStackHi = 0;   // the sampled thread's stack

static DWORD WINAPI SampleMain(void*)
{
    while (WaitForSingleObject(GSampleQuit, 1) == WAIT_TIMEOUT)
    {
        if (GSampleCount >= SamplesMost || SuspendThread(GSampled) == (DWORD)-1)
            continue;
        CONTEXT C;
        C.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(GSampled, &C))
        {
            DWORD Caller = 0;
            // In the C library: where the call came from. Its routines keep a frame on EBP, so the chain of saved EBPs is
            // followed, within the thread's stack, to the first return address into the tool's own code; failing that,
            // the first such word in the top few of the stack (a routine with no frame of its own).
            if (C.Eip < GImageLo || C.Eip >= GImageHi)
            {
                DWORD Frame = C.Ebp;
                for (int Depth = 0; Depth < 16 && !Caller && Frame >= C.Esp && Frame >= GStackLo && Frame + 8 <= GStackHi;
                     Depth++)
                {
                    const DWORD Ret = ((const DWORD*)(UINT_PTR)Frame)[1];
                    if (Ret >= GImageLo && Ret < GImageHi)
                        Caller = Ret;
                    const DWORD Next = ((const DWORD*)(UINT_PTR)Frame)[0];
                    if (Next <= Frame)
                        break;
                    Frame = Next;
                }
                for (int k = 0; k < 8 && !Caller; k++)
                {
                    const DWORD W = ((const DWORD*)(UINT_PTR)C.Esp)[k];
                    if (W >= GImageLo && W < GImageHi)
                        Caller = W;
                }
            }
            GCallers[GSampleCount] = Caller;
            GSamples[GSampleCount] = C.Eip;
            GSampleCount++;
        }
        ResumeThread(GSampled);
    }
    return 0;
}

static void SampleReport(int Top)
{
    const LONG   N       = GSampleCount;
    const HANDLE Process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(Process, nullptr, TRUE))
    {
        printf("sample: no symbols\n");
        return;
    }
    std::vector<std::pair<DWORD, DWORD>> Pairs((size_t)N);
    for (LONG i = 0; i < N; i++)
        Pairs[(size_t)i] = std::make_pair(GSamples[i], GCallers[i]);
    std::sort(Pairs.begin(), Pairs.end());
    std::map<std::string, long> Outer, Inner, Lines, Calls;
    char         Buf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* Sym = (SYMBOL_INFO*)Buf;
    // A place's function, its innermost inlined function and its line.
    auto Name = [&](DWORD64 At, std::string& OuterName, std::string& InnerName, std::string& LineName)
    {
        OuterName = "?";
        InnerName.clear();
        LineName = "?";
        memset(Buf, 0, sizeof(Buf));
        Sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        Sym->MaxNameLen   = 511;
        DWORD64 Off       = 0;
        if (SymFromAddr(Process, At, &Off, Sym))
            OuterName = Sym->Name;
        // Outside the tool's code: the module and how far past its nearest export, which names a body only roughly.
        if (At < GImageLo || At >= GImageHi)
        {
            HMODULE Mod = nullptr;
            char    Path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(UINT_PTR)At, &Mod) && Mod)
                GetModuleFileNameA(Mod, Path, MAX_PATH);
            const char* Base = Path;
            for (const char* p = Path; *p; p++)
                if (*p == '\\' || *p == '/')
                    Base = p + 1;
            char M[400];
            sprintf(M, "%s!%s+0x%llx", Base, OuterName.c_str(), (unsigned long long)(Off & ~0xFull));
            OuterName = M;
        }
        IMAGEHLP_LINE64 Line;
        memset(&Line, 0, sizeof(Line));
        Line.SizeOfStruct = sizeof(Line);
        DWORD LineOff     = 0;
        // The innermost inlined function and its line, where the code at the place was inlined.
        const DWORD Inlines = SymAddrIncludeInlineTrace(Process, At);
        DWORD       Context = 0, Frame = 0;
        bool        bLine   = false;
        if (Inlines > 0 && SymQueryInlineTrace(Process, At, 0, At, At, &Context, &Frame))
        {
            memset(Buf, 0, sizeof(Buf));
            Sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            Sym->MaxNameLen   = 511;
            if (SymFromInlineContext(Process, At, Context, &Off, Sym))
                InnerName = Sym->Name;
            bLine = SymGetLineFromInlineContext(Process, At, Context, 0, &LineOff, &Line) != FALSE;
        }
        if (!bLine)
            bLine = SymGetLineFromAddr64(Process, At, &LineOff, &Line) != FALSE;
        if (bLine)
        {
            const char* File = Line.FileName;
            for (const char* p = Line.FileName; *p; p++)
                if (*p == '\\' || *p == '/')
                    File = p + 1;
            char L[300];
            sprintf(L, "%s:%lu", File, Line.LineNumber);
            LineName = L;
        }
    };
    for (LONG i = 0; i < N;)
    {
        LONG j = i + 1;
        while (j < N && Pairs[(size_t)j] == Pairs[(size_t)i])
            j++;
        std::string OuterName, InnerName, LineName;
        Name(Pairs[(size_t)i].first, OuterName, InnerName, LineName);
        Outer[OuterName] += j - i;
        Inner[InnerName.empty() ? OuterName : InnerName] += j - i;
        Lines[LineName] += j - i;
        if (Pairs[(size_t)i].second)
        {
            std::string CallerOuter, CallerInner, CallerLine;
            // The return address is the instruction after the call: one byte back is the call itself.
            Name(Pairs[(size_t)i].second - 1, CallerOuter, CallerInner, CallerLine);
            Calls[OuterName + " <- " + CallerLine + " (" + (CallerInner.empty() ? CallerOuter : CallerInner) + ")"] +=
                j - i;
        }
        i = j;
    }
    auto Print = [&](const char* Title, const std::map<std::string, long>& M, int Most)
    {
        std::vector<std::pair<long, std::string>> V;
        for (const auto& KV : M)
            V.push_back(std::make_pair(KV.second, KV.first));
        std::sort(V.begin(), V.end(), [](const std::pair<long, std::string>& A, const std::pair<long, std::string>& B)
                  { return A.first > B.first; });
        printf("sample: %s, %ld samples\n", Title, (long)N);
        for (int k = 0; k < (int)V.size() && k < Most; k++)
            printf("  %5.1f%%  %6ld  %s\n", 100.0 * V[k].first / (N ? N : 1), V[k].first, V[k].second.c_str());
    };
    Print("by function", Outer, Top);
    Print("by the innermost inlined function", Inner, Top);
    Print("by line", Lines, Top * 2);
    Print("the C library's, by where it was called from", Calls, Top * 2);
    SymCleanup(Process);
}
#endif

using RdxSphere::FPolyIn;
using RdxSphere::FResult;
using RdxSphere::FV3;
using RdxSphere::TList;

static double Now()
{
    LARGE_INTEGER Freq, T;
    QueryPerformanceFrequency(&Freq);
    QueryPerformanceCounter(&T);
    return (double)T.QuadPart / (double)Freq.QuadPart;
}

// LRC_MEMLAPS=1: the rounding's clock (FResult::Seconds) also notes the memory in use each time it is read - at the start
// and at the end of each of its parts -, for how much each part holds.
static double GMemLapMB[64];
static int    GMemLaps = 0;
static double NowMem()
{
    PROCESS_MEMORY_COUNTERS Mc;
    Mc.cb = sizeof(Mc);
    if (GMemLaps < 64 && GetProcessMemoryInfo(GetCurrentProcess(), &Mc, sizeof(Mc)))
        GMemLapMB[GMemLaps++] = Mc.PagefileUsage / 1048576.0;
    return Now();
}

static bool ReadAll(FILE* F, void* Data, size_t Bytes) { return Bytes == 0 || fread(Data, 1, Bytes, F) == Bytes; }

using RdxLevelRound::PackedMoveAt;

// SolidPass.hlsl's RoundQuadUv, in floats as the card has it: where W lies in the quad Q laid flat along Drop.
static bool QuadUvFloat(const FV3* Qd, int Drop, const FV3& Wd, float& Uo, float& Vo)
{
    float Q[4][3], W[3];
    for (int k = 0; k < 4; k++)
    {
        Q[k][0] = (float)Qd[k].X;
        Q[k][1] = (float)Qd[k].Y;
        Q[k][2] = (float)Qd[k].Z;
    }
    W[0] = (float)Wd.X;
    W[1] = (float)Wd.Y;
    W[2] = (float)Wd.Z;
    const int ix = (Drop == 0) ? 1 : 0, iy = (Drop == 2) ? 1 : 2;
    const float Ex = Q[1][ix] - Q[0][ix], Ey = Q[1][iy] - Q[0][iy];
    const float Fx = Q[3][ix] - Q[0][ix], Fy = Q[3][iy] - Q[0][iy];
    const float Gx = Q[0][ix] - Q[1][ix] + Q[2][ix] - Q[3][ix], Gy = Q[0][iy] - Q[1][iy] + Q[2][iy] - Q[3][iy];
    const float Hx = W[ix] - Q[0][ix], Hy = W[iy] - Q[0][iy];
    const float K2 = Gx * Fy - Gy * Fx, K1 = Ex * Fy - Ey * Fx + Hx * Gy - Hy * Gx;
    const float K0 = Hx * Ey - Hy * Ex;
    if (!(fabsf(Ex * Fy - Ey * Fx) > 1e-12f))
        return false;
    const float Wq = K1 * K1 - 4.0f * K0 * K2;
    if (Wq < 0.0f)
        return false;
    const float Qr = -0.5f * (K1 + ((K1 < 0.0f) ? -sqrtf(Wq) : sqrtf(Wq)));
    if (Qr == 0.0f)
        return false;
    const float V1 = K0 / Qr, V2 = (K2 != 0.0f) ? Qr / K2 : 1e30f;
    const float D1x = Ex + Gx * V1, D1y = Ey + Gy * V1, D2x = Ex + Gx * V2, D2y = Ey + Gy * V2;
    const float U1 = (fabsf(D1x) >= fabsf(D1y)) ? (Hx - Fx * V1) / D1x : (Hy - Fy * V1) / D1y;
    const float U2 = (fabsf(D2x) >= fabsf(D2y)) ? (Hx - Fx * V2) / D2x : (Hy - Fy * V2) / D2y;
    const float Off1 = fmaxf(fmaxf(-V1, V1 - 1.0f), fmaxf(-U1, U1 - 1.0f));
    const float Off2 = (V2 < 1e29f) ? fmaxf(fmaxf(-V2, V2 - 1.0f), fmaxf(-U2, U2 - 1.0f)) : 1e30f;
    Uo = (Off1 <= Off2) ? U1 : U2;
    Vo = (Off1 <= Off2) ? V1 : V2;
    return true;
}

// Keys sorted once filled - a few bytes a key, where a hash set takes tens: the bevel check of the largest levels
// holds millions of sides.
struct FKeySet
{
    std::vector<long long> Keys;
    void Add(long long K) { Keys.push_back(K); }
    void Done()
    {
        std::sort(Keys.begin(), Keys.end());
        Keys.erase(std::unique(Keys.begin(), Keys.end()), Keys.end());
    }
    bool Has(long long K) const { return std::binary_search(Keys.begin(), Keys.end(), K); }
};
// Cells to items, the same way.
struct FCellMap
{
    typedef std::pair<long long, int> FItem;
    std::vector<FItem> Items;
    void Add(long long K, int I) { Items.push_back(FItem(K, I)); }
    void Done() { std::sort(Items.begin(), Items.end()); }
    struct FIt
    {
        const FItem* P;
        int          operator*() const { return P->second; }
        FIt&         operator++()
        {
            ++P;
            return *this;
        }
        bool operator!=(const FIt& O) const { return P != O.P; }
    };
    struct FRange
    {
        FIt B, E;
        FIt begin() const { return B; }
        FIt end() const { return E; }
    };
    FRange At(long long K) const
    {
        const FItem* const First = Items.data();
        const FItem* const Last  = First + Items.size();
        const FItem*       Lo    = std::lower_bound(First, Last, FItem(K, INT_MIN));
        const FItem*       Hi    = Lo;
        while (Hi != Last && Hi->first == K)
            ++Hi;
        return FRange{ FIt{ Lo }, FIt{ Hi } };
    }
};

// LRC_ROOM=<MB>: the rounding and the bevel given that much memory from where the rounding begins - a list's block past
// it refused (RdxSphere::RoomFor), as the game refuses one that would leave it too little address space: their ways out
// with no memory walked, wherever the block comes.
static double GRoomFrom = 0.0, GRoomMB = 0.0;
static bool RoomWithin(size_t Bytes)
{
    PROCESS_MEMORY_COUNTERS Mc = {};
    Mc.cb                      = sizeof(Mc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &Mc, sizeof(Mc)))
        return true;
    return Mc.PagefileUsage / 1048576.0 - GRoomFrom + Bytes / 1048576.0 <= GRoomMB;
}

int main(int Argc, char** Argv)
{
    if (Argc < 2)
    {
        printf("usage: LevelRoundCheck <in> [angle]\n");
        return 2;
    }
    const double Angle = (Argc > 2) ? atof(Argv[2]) : 105.0;
    // LRC_BEVEL_TRACE=<w>: the bevel's move of place w, and the faces that keep it in their planes.
    if (const char* T = getenv("LRC_BEVEL_TRACE"))
        RdxLevelRound::BevelDetail::LevelBevelTracePlace() = atoi(T);
    // LRC_BEVEL_FACETS_OFF=1: the bands cut only the faces in an edge's face's plane, not the curved walls' facets
    // beside them (the bevel's A/B).
    RdxLevelRound::BevelDetail::LevelBevelFacetsOff() = getenv("LRC_BEVEL_FACETS_OFF") != nullptr;
    FILE*        F     = fopen(Argv[1], "rb");
    if (!F)
    {
        printf("cannot read %s\n", Argv[1]);
        return 2;
    }
    char   Magic[4];
    double Cap   = 0.0;
    int    NumPts = 0, NumPolys = 0;
    TList<FV3>     Pts;
    TList<FPolyIn> Polys;
    bool bOk = ReadAll(F, Magic, 4) && memcmp(Magic, "SPI1", 4) == 0 && ReadAll(F, &Cap, sizeof(Cap)) &&
               ReadAll(F, &NumPts, sizeof(int)) && NumPts >= 0 && Pts.Resize(NumPts) &&
               ReadAll(F, Pts.Data, sizeof(FV3) * (size_t)NumPts) && ReadAll(F, &NumPolys, sizeof(int)) &&
               NumPolys >= 0 && Polys.Resize(NumPolys);
    for (int p = 0; p < NumPolys && bOk; p++)
    {
        FPolyIn& P = Polys[p];
        bOk = ReadAll(F, &P.First, sizeof(int)) && ReadAll(F, &P.Count, sizeof(int)) && ReadAll(F, &P.Nx, sizeof(double)) &&
              ReadAll(F, &P.Ny, sizeof(double)) && ReadAll(F, &P.Nz, sizeof(double)) && ReadAll(F, &P.W, sizeof(double)) &&
              ReadAll(F, &P.Use, sizeof(int));
    }
    fclose(F);
    if (!bOk)
    {
        printf("bad input %s\n", Argv[1]);
        return 2;
    }

    const char* const ReliefAll = getenv("LRC_RELIEF");
    if (ReliefAll && atoi(ReliefAll) != 0)
        for (int p = 0; p < NumPolys; p++)
            Polys[p].Use |= (Polys[p].Use & RdxSphere::Use_Wall) ? RdxSphere::Use_Relief : 0;

    // LRC_TILES=<n>: the level laid n times along X and n times along Y, each copy a margin past the one before - a level
    // that many times as large, for how the memory and the time grow with the level.
    if (const char* TilesText = getenv("LRC_TILES"))
    {
        const int n = atoi(TilesText);
        if (n > 1 && NumPts > 0)
        {
            FV3 Lo = Pts[0], Hi = Pts[0];
            for (int v = 1; v < NumPts; v++)
            {
                Lo = RdxSphere::V3(RdxSphere::MinOf(Lo.X, Pts[v].X), RdxSphere::MinOf(Lo.Y, Pts[v].Y), Lo.Z);
                Hi = RdxSphere::V3(RdxSphere::MaxOf(Hi.X, Pts[v].X), RdxSphere::MaxOf(Hi.Y, Pts[v].Y), Hi.Z);
            }
            const double   Wx = Hi.X - Lo.X + 1024.0, Wy = Hi.Y - Lo.Y + 1024.0;
            TList<FV3>     MorePts;
            TList<FPolyIn> MorePolys;
            if (!MorePts.Resize(NumPts * n * n) || !MorePolys.Resize(NumPolys * n * n))
            {
                printf("no memory for the tiles\n");
                return 2;
            }
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++)
                {
                    const int k   = j * n + i;
                    const FV3 Off = RdxSphere::V3(i * Wx, j * Wy, 0.0);
                    for (int v = 0; v < NumPts; v++)
                        MorePts[k * NumPts + v] = Pts[v] + Off;
                    for (int p = 0; p < NumPolys; p++)
                    {
                        FPolyIn Q = Polys[p];
                        Q.First += k * NumPts;
                        Q.W += Q.Nx * Off.X + Q.Ny * Off.Y + Q.Nz * Off.Z;
                        MorePolys[k * NumPolys + p] = Q;
                    }
                }
            RdxLevelRound::Detail::HandOver(Pts, MorePts);
            RdxLevelRound::Detail::HandOver(Polys, MorePolys);
            NumPts   = Pts.Num;
            NumPolys = Polys.Num;
            printf("tiles: the level laid %d by %d - %d places, %d polygons\n", n, n, NumPts, NumPolys);
        }
    }
    if (const char* Box = getenv("LRC_TRACE_BOX"))
        sscanf(Box, "%lf,%lf,%lf,%lf,%lf,%lf", &GTraceBox[0], &GTraceBox[1], &GTraceBox[2], &GTraceBox[3], &GTraceBox[4],
               &GTraceBox[5]);
    FResult R;
    // LRC_WORKERS=<n>: the rounding on n threads (one: the caller's alone); none, as many as it may take where it is
    // waited for (RdxSphere::WorkersAvailable). What it makes must not change with it.
    R.Workers = RdxSphere::WorkersAvailable();
    if (const char* Workers = getenv("LRC_WORKERS"))
        R.Workers = (atoi(Workers) > 0) ? atoi(Workers) : 1;
    // LRC_SAG=<units>: how far a cut piece of a curve may stand off it, as one of the spherifier's levels asks
    // (RenderMath.h's SpherifierSagOf); none, the High one.
    if (const char* SagText = getenv("LRC_SAG"))
        R.Sag = (atof(SagText) > 0.0) ? (float)atof(SagText) : R.Sag;
#if !defined(_WIN64)
    if (const char* Sample = getenv("LRC_SAMPLE"))
    {
        const int Rounds = (atoi(Sample) > 0) ? atoi(Sample) : 1;
        GSamples         = (DWORD*)VirtualAlloc(nullptr, SamplesMost * sizeof(DWORD), MEM_COMMIT | MEM_RESERVE,
                                                PAGE_READWRITE);
        GCallers         = (DWORD*)VirtualAlloc(nullptr, SamplesMost * sizeof(DWORD), MEM_COMMIT | MEM_RESERVE,
                                                PAGE_READWRITE);
        GSampleQuit      = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        {
            // The tool's code: its executable section.
            const HMODULE               Image = GetModuleHandleW(nullptr);
            const IMAGE_DOS_HEADER*     Dos   = (const IMAGE_DOS_HEADER*)Image;
            const IMAGE_NT_HEADERS*     Nt    = (const IMAGE_NT_HEADERS*)((const char*)Image + Dos->e_lfanew);
            const IMAGE_SECTION_HEADER* Sec   = IMAGE_FIRST_SECTION(Nt);
            for (int s = 0; s < Nt->FileHeader.NumberOfSections; s++)
                if (Sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)
                {
                    GImageLo = (DWORD)(UINT_PTR)Image + Sec[s].VirtualAddress;
                    GImageHi = GImageLo + Sec[s].Misc.VirtualSize;
                    break;
                }
            const NT_TIB* Tib = (const NT_TIB*)NtCurrentTeb();
            GStackLo          = (DWORD)(UINT_PTR)Tib->StackLimit;
            GStackHi          = (DWORD)(UINT_PTR)Tib->StackBase;
        }
        if (GSamples && GCallers && GSampleQuit &&
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &GSampled,
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        {
            timeBeginPeriod(1);
            HANDLE Sampler = CreateThread(nullptr, 0, &SampleMain, nullptr, 0, nullptr);
            if (Sampler)
                SetThreadPriority(Sampler, THREAD_PRIORITY_TIME_CRITICAL);
            for (int k = 0; k < Rounds; k++)
                RdxLevelRound::Round(Pts.Data, NumPts, Polys.Data, NumPolys, Cap, Angle, R);
            SetEvent(GSampleQuit);
            if (Sampler)
            {
                WaitForSingleObject(Sampler, 5000);
                CloseHandle(Sampler);
            }
            timeEndPeriod(1);
            SampleReport(30);
        }
        GSheetsHeld = 0;
    }
#endif
    {
        PROCESS_MEMORY_COUNTERS Mc = {};
        Mc.cb                      = sizeof(Mc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &Mc, sizeof(Mc)))
            printf("memory before the rounding: %.0f MB\n", Mc.PagefileUsage / 1048576.0);
        if (const char* RoomText = getenv("LRC_ROOM"))
        {
            GRoomMB              = atof(RoomText);
            GRoomFrom            = Mc.PagefileUsage / 1048576.0;
            RdxSphere::RoomFor() = RoomWithin;
            printf("room: %.0f MB from here on\n", GRoomMB);
        }
    }
    const double T0    = Now();
    R.Seconds          = getenv("LRC_MEMLAPS") ? NowMem : Now;
    // LRC_REGION_ALONE=1 (with "region"): the whole level is not rounded - only the cubes are, each alone, for the most
    // memory one of them takes (the process's peak after each); their triangles are then held against nothing.
    const bool   bMade = (getenv("LRC_REGION_ALONE") && Argc > 3 && strcmp(Argv[3], "region") == 0) ||
                       RdxLevelRound::Round(Pts.Data, NumPts, Polys.Data, NumPolys, Cap, Angle, R);
    const double Ms    = (Now() - T0) * 1000.0;
    if (!bMade)
    {
        printf("no memory\n");
        return 1;
    }
    printf("result hash: %016llx (on %d threads)\n", ResultHash(R), R.Workers);
    {
        PROCESS_MEMORY_COUNTERS Mc = {};
        Mc.cb                      = sizeof(Mc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &Mc, sizeof(Mc)))
            printf("memory after the rounding: %.0f MB now, %.0f MB at the most; the result holds %.0f MB\n",
                   Mc.PagefileUsage / 1048576.0, Mc.PeakPagefileUsage / 1048576.0, (double)R.Bytes() / 1048576.0);
        if (GMemLaps > 0)
        {
            printf("memory at the rounding's clock (MB, start then each part's end):");
            for (int i = 0; i < GMemLaps; i++)
                printf(" %.0f", GMemLapMB[i]);
            printf("\n");
        }
    }
    // LRC_RENDER: a picture of the rounding (and of each variant below, and of the bevel's in "bevel" mode).
    double RenderCam[6];
    int    RenderW = 0, RenderH = 0;
    char   RenderPrefix[512];
    const bool bRender = RenderAsked(RenderCam, RenderW, RenderH, RenderPrefix, sizeof(RenderPrefix));
    if (bRender)
    {
        char Name[600];
        snprintf(Name, sizeof(Name), "%s-rounded.ppm", RenderPrefix);
        RenderResult(R, Pts.Data, Polys.Data, NumPolys, RenderCam, RenderW, RenderH, Name);
    }
    // LRC_VARIANT_CUBE=<units>: the level rounded once more with the flat polygons' pieces kept within cubes of that
    // size (FResult::PieceCube) - how a rounding made a cube at a time would look -, its drawn surface set against this
    // one's (SurfacesApart). LRC_VARIANT_ONLY=1: nothing else after it.
    if (const char* CubeText = getenv("LRC_VARIANT_CUBE"))
    {
        FResult V;
        V.Workers   = R.Workers;
        V.Sag       = R.Sag;
        V.PieceCube = atof(CubeText);
        const double Tv = Now();
        if (!RdxLevelRound::Round(Pts.Data, NumPts, Polys.Data, NumPolys, Cap, Angle, V))
            printf("variant: no memory\n");
        else
        {
            char What[64];
            snprintf(What, sizeof(What), "pieces within cubes of %.0f", V.PieceCube);
            printf("%s: made in %.0f ms; pieces %d (as drawn %d), places %d (%d)\n", What, (Now() - Tv) * 1000.0, V.Pieces,
                   R.Pieces, V.Orig.Num, R.Orig.Num);
            SurfacesApart(R, V, Pts.Data, Polys.Data, NumPolys, What);
            if (bRender)
            {
                char Name[600];
                snprintf(Name, sizeof(Name), "%s-cube.ppm", RenderPrefix);
                RenderResult(V, Pts.Data, Polys.Data, NumPolys, RenderCam, RenderW, RenderH, Name);
            }
        }
        if (getenv("LRC_VARIANT_ONLY"))
            return 0;
    }
    // "region <cell> <halo>": the level in cubes of <cell> units, a polygon the cube's whose corners' middle lies in it;
    // the cubes with the most polygons (LRC_REGIONS of them, six if none) each rounded alone - its polygons and every
    // polygon whose box comes within <halo> of the cube -, and its polygons' triangles set against the whole level's:
    // how many have other triangles, and how far the nearest of the level's corners is from each of theirs at most.
    // Whether a level can be rounded a piece at a time, and with how much round each piece.
    if (Argc > 5 && strcmp(Argv[3], "region") == 0)
    {
        const double CellSize = atof(Argv[4]), Halo = atof(Argv[5]);
        const int    Most     = getenv("LRC_REGIONS") ? atoi(getenv("LRC_REGIONS")) : 6;
        std::vector<FV3> Lo(NumPolys), Hi(NumPolys);
        std::vector<long long> Owner(NumPolys);
        std::map<long long, std::vector<int>> Cubes;
        for (int q = 0; q < NumPolys; q++)
        {
            const FPolyIn& P = Polys[q];
            FV3 Mid = RdxSphere::V3(0.0, 0.0, 0.0);
            Lo[q] = Hi[q] = (P.Count > 0) ? Pts[P.First] : Mid;
            for (int k = 0; k < P.Count; k++)
            {
                const FV3& X = Pts[P.First + k];
                Mid          = Mid + X * (1.0 / P.Count);
                Lo[q]        = RdxSphere::V3(RdxSphere::MinOf(Lo[q].X, X.X), RdxSphere::MinOf(Lo[q].Y, X.Y), RdxSphere::MinOf(Lo[q].Z, X.Z));
                Hi[q]        = RdxSphere::V3(RdxSphere::MaxOf(Hi[q].X, X.X), RdxSphere::MaxOf(Hi[q].Y, X.Y), RdxSphere::MaxOf(Hi[q].Z, X.Z));
            }
            Owner[q] = RdxLevelRound::Detail::CellKey((long long)floor(Mid.X / CellSize), (long long)floor(Mid.Y / CellSize),
                                                      (long long)floor(Mid.Z / CellSize));
            Cubes[Owner[q]].push_back(q);
        }
        // LRC_HALO_FLAT=1: the round by the level's flat polygons instead of by distance alone - each polygon of the cube
        // with every polygon of its flat polygon (polygons sharing a side in one plane), every polygon sharing a place
        // with those, and their flat polygons in turn; the distance's round as well, for the holds near.
        const bool bFlatHalo = getenv("LRC_HALO_FLAT") != nullptr;
        std::vector<int> FlatOf(NumPolys);
        std::vector<unsigned char> FlatSoft(NumPolys, 0);   // a flat polygon (by its root) with a soft side: it bends
        std::unordered_map<long long, std::vector<int>> PolysAt;   // a place by its position to the 64th: its polygons
        if (bFlatHalo)
        {
            auto PlaceKey = [](const FV3& X) {
                return RdxLevelRound::Detail::CellKey((long long)floor(X.X * 64.0 + 0.5), (long long)floor(X.Y * 64.0 + 0.5),
                                                      (long long)floor(X.Z * 64.0 + 0.5));
            };
            std::vector<int> Up(NumPolys);
            for (int q = 0; q < NumPolys; q++)
                Up[q] = q;
            auto Root = [&](int q) {
                while (Up[q] != q)
                    q = Up[q] = Up[Up[q]];
                return q;
            };
            std::unordered_map<unsigned long long, std::vector<int>> SideOf;
            for (int q = 0; q < NumPolys; q++)
            {
                const FPolyIn& P = Polys[q];
                for (int k = 0; k < P.Count; k++)
                {
                    const long long A = PlaceKey(Pts[P.First + k]), B = PlaceKey(Pts[P.First + (k + 1) % P.Count]);
                    PolysAt[A].push_back(q);
                    const unsigned long long K = (unsigned long long)((A < B) ? A : B) * 1000003ull ^ (unsigned long long)((A < B) ? B : A);
                    SideOf[K].push_back(q);
                }
            }
            for (const auto& S : SideOf)
                for (size_t i = 0; i < S.second.size(); i++)
                    for (size_t j = i + 1; j < S.second.size(); j++)
                    {
                        const FPolyIn& A = Polys[S.second[i]];
                        const FPolyIn& B = Polys[S.second[j]];
                        if (A.Nx * B.Nx + A.Ny * B.Ny + A.Nz * B.Nz >= 0.99996 && fabs(A.W - B.W) <= 0.01)
                            Up[Root(S.second[i])] = Root(S.second[j]);
                    }
            for (int q = 0; q < NumPolys; q++)
                FlatOf[q] = Root(q);
            // A side between two flat polygons turning by less than the soft turn (Angle's): both may bend.
            const double SoftCos = cos((180.0 - Angle) * 3.14159265358979 / 180.0);
            for (const auto& S : SideOf)
                for (size_t i = 0; i < S.second.size(); i++)
                    for (size_t j = i + 1; j < S.second.size(); j++)
                    {
                        const FPolyIn& A = Polys[S.second[i]];
                        const FPolyIn& B = Polys[S.second[j]];
                        if (FlatOf[S.second[i]] != FlatOf[S.second[j]] && A.Nx * B.Nx + A.Ny * B.Ny + A.Nz * B.Nz >= SoftCos)
                            FlatSoft[FlatOf[S.second[i]]] = FlatSoft[FlatOf[S.second[j]]] = 1;
                    }
            std::unordered_map<int, std::vector<int>> Members;
            for (int q = 0; q < NumPolys; q++)
                Members[FlatOf[q]].push_back(q);
            // Kept for the cubes: the flat polygons' members, by root.
            for (auto& M : Members)
                std::sort(M.second.begin(), M.second.end());
            static std::unordered_map<int, std::vector<int>> GMembers;
            GMembers.swap(Members);
            (void)GMembers;
        }
        std::vector<std::pair<int, long long>> Order;
        for (const auto& C : Cubes)
            Order.push_back({ -(int)C.second.size(), C.first });
        std::sort(Order.begin(), Order.end());
        // LRC_REGION_BEVEL=1: the whole level's fine bevel first, and each cube's too - the cubes' polygons held against
        // the level's bevelled.
        // LRC_REGION_INDEX=1: each cube's polygons by LevelRegion.h (the renderer's), held against the ones made here
        // (the LRC_HALO_* experiments) where those are asked for too, and taken for the work.
        const bool                  bRegionIndex = getenv("LRC_REGION_INDEX") != nullptr;
        RdxLevelRound::FRegionIndex RegionIndex;
        if (bRegionIndex)
        {
            const double Ti = Now();
            if (!RdxLevelRound::RegionIndexMake(Pts.Data, Pts.Num, Polys.Data, NumPolys, Angle, RegionIndex))
            {
                printf("region: no memory for the index\n");
                return 1;
            }
            printf("region: the index made in %.0f ms (%d polygons boxed in no cell: %d)\n", (Now() - Ti) * 1000.0,
                   NumPolys, RegionIndex.Big.Num);
            // Its flat polygons against the ones made here: how many polygons are in one of another size.
            if (bFlatHalo)
            {
                std::unordered_map<int, int> Size;
                for (int q = 0; q < NumPolys; q++)
                    Size[FlatOf[q]]++;
                int Other = 0, First = -1;
                for (int q = 0; q < NumPolys; q++)
                {
                    const int f = RegionIndex.FlatOf[q];
                    if (Size[FlatOf[q]] != RegionIndex.FlatStart[f + 1] - RegionIndex.FlatStart[f])
                    {
                        Other++;
                        if (First < 0)
                            First = q;
                    }
                }
                printf("region: the index's flat polygons - %d polygons in one of another size than here (the first %d: "
                       "here %d, the index %d)\n", Other, First, First >= 0 ? Size[FlatOf[First]] : 0,
                       First >= 0 ? RegionIndex.FlatStart[RegionIndex.FlatOf[First] + 1] -
                                        RegionIndex.FlatStart[RegionIndex.FlatOf[First]]
                                  : 0);
                // The sides here are told by a hash of their two places: those that share one with another side.
                std::unordered_map<unsigned long long, std::vector<std::pair<long long, long long>>> ByHash;
                auto Key64 = [](const FV3& X) {
                    return RdxLevelRound::Detail::CellKey((long long)floor(X.X * 64.0 + 0.5), (long long)floor(X.Y * 64.0 + 0.5),
                                                          (long long)floor(X.Z * 64.0 + 0.5));
                };
                int Collide = 0;
                for (int q = 0; q < NumPolys; q++)
                {
                    const FPolyIn& P = Polys[q];
                    for (int k = 0; k < P.Count; k++)
                    {
                        const long long A = Key64(Pts[P.First + k]), B = Key64(Pts[P.First + (k + 1) % P.Count]);
                        const long long L2 = (A < B) ? A : B, H2 = (A < B) ? B : A;
                        const unsigned long long K = (unsigned long long)L2 * 1000003ull ^ (unsigned long long)H2;
                        auto& V = ByHash[K];
                        bool bHad = false;
                        for (const auto& E : V)
                            bHad = bHad || (E.first == L2 && E.second == H2);
                        if (!bHad)
                        {
                            Collide += V.empty() ? 0 : 1;
                            V.push_back({ L2, H2 });
                        }
                    }
                }
                printf("region: sides whose hash here is another side's: %d\n", Collide);
            }
        }
        const bool bRegionBevel = getenv("LRC_REGION_BEVEL") != nullptr;
        if (bRegionBevel)
        {
            RdxLevelRound::FBevelStats Sw;
            RdxLevelRound::FBevelMade  Bw;
            RdxLevelRound::Bevel(Pts.Data, Pts.Num, Polys.Data, NumPolys, nullptr, nullptr, 0, Angle, 1.0, R, Sw, Bw);
            printf("region: the whole level bevelled - %d polygons cut\n", Bw.Polygons);
        }
        printf("region: %d polygons in %d cubes of %.0f, %.0f round each\n", NumPolys, (int)Cubes.size(), CellSize, Halo);
        for (int c = 0; c < (int)Order.size() && c < Most; c++)
        {
            const std::vector<int>& Own = Cubes[Order[c].second];
            // The cube's box: from its first polygon's corners' middle.
            FV3 Mid0 = RdxSphere::V3(0.0, 0.0, 0.0);
            {
                const FPolyIn& P = Polys[Own[0]];
                for (int k = 0; k < P.Count; k++)
                    Mid0 = Mid0 + Pts[P.First + k] * (1.0 / P.Count);
            }
            const FV3 CubeLo = RdxSphere::V3(floor(Mid0.X / CellSize) * CellSize, floor(Mid0.Y / CellSize) * CellSize,
                                             floor(Mid0.Z / CellSize) * CellSize);
            const FV3 BoxLo = CubeLo - RdxSphere::V3(Halo, Halo, Halo);
            const FV3 BoxHi = CubeLo + RdxSphere::V3(CellSize + Halo, CellSize + Halo, CellSize + Halo);
            TList<FV3>     SubPts;
            TList<FPolyIn> SubPolys;
            std::vector<int> Orig, SubOf(NumPolys, -1);
            std::vector<unsigned char> In(NumPolys, 0);
            if (bFlatHalo)
            {
                auto PlaceKey = [](const FV3& X) {
                    return RdxLevelRound::Detail::CellKey((long long)floor(X.X * 64.0 + 0.5), (long long)floor(X.Y * 64.0 + 0.5),
                                                          (long long)floor(X.Z * 64.0 + 0.5));
                };
                std::unordered_map<int, std::vector<int>> Members;
                for (int q = 0; q < NumPolys; q++)
                    Members[FlatOf[q]].push_back(q);
                // LRC_HALO_FLATMAX=<units>: of a flat polygon only its polygons whose box comes within that of the cube.
                const double FlatMax = getenv("LRC_HALO_FLATMAX") ? atof(getenv("LRC_HALO_FLATMAX")) : 1e30;
                auto AddFlats = [&](const std::vector<int>& From) {
                    std::vector<int> Out;
                    for (const int q : From)
                        if (FlatSoft[FlatOf[q]] || !getenv("LRC_HALO_SOFT"))
                        for (const int m : Members[FlatOf[q]])
                            if (!In[m] && Lo[m].X <= CubeLo.X + CellSize + FlatMax && Hi[m].X >= CubeLo.X - FlatMax &&
                                Lo[m].Y <= CubeLo.Y + CellSize + FlatMax && Hi[m].Y >= CubeLo.Y - FlatMax &&
                                Lo[m].Z <= CubeLo.Z + CellSize + FlatMax && Hi[m].Z >= CubeLo.Z - FlatMax)
                            {
                                In[m] = 1;
                                Out.push_back(m);
                            }
                    return Out;
                };
                std::vector<int> Step1 = AddFlats(Own);
                for (const int q : Own)
                    In[q] = 1;
                std::vector<int> Ring1;
                for (int q = 0; q < NumPolys; q++)
                    if (In[q])
                    {
                        const FPolyIn& P = Polys[q];
                        for (int k = 0; k < P.Count; k++)
                            for (const int m : PolysAt[PlaceKey(Pts[P.First + k])])
                                Ring1.push_back(m);
                    }
                AddFlats(Ring1);
                // LRC_HALO_RING2=1: and every polygon sharing a place with those - the faces round their places, whose
                // normals their edges bend by.
                if (getenv("LRC_HALO_RING2"))
                {
                    std::vector<int> Ring2;
                    for (int q = 0; q < NumPolys; q++)
                        if (In[q])
                        {
                            const FPolyIn& P = Polys[q];
                            for (int k = 0; k < P.Count; k++)
                                for (const int m : PolysAt[PlaceKey(Pts[P.First + k])])
                                    Ring2.push_back(m);
                        }
                    for (const int m : Ring2)
                        In[m] = 1;
                }
                // LRC_HALO_RUNS=1: and every polygon with a side on the line of a hard side of one taken (two polygons
                // across it turning further than the soft turn) - the bevel's edges are runs of such sides along a line.
                // LRC_HALO_RUNS=2: once more after the near ones are taken.
                auto RunsOnce = [&]()
                {
                    const double SoftCosR = cos((180.0 - Angle) * 3.14159265358979 / 180.0);
                    auto LineKey = [](const FV3& A, const FV3& B) {
                        FV3          D = B - A;
                        const double L = RdxSphere::Length(D);
                        if (!(L > 1e-6))
                            return (long long)0;
                        D = D * (1.0 / L);
                        if (D.X < -1e-9 || (fabs(D.X) <= 1e-9 && (D.Y < -1e-9 || (fabs(D.Y) <= 1e-9 && D.Z < 0.0))))
                            D = D * -1.0;
                        const FV3 P = A - D * RdxSphere::Dot(A, D);
                        const long long H1 = RdxLevelRound::Detail::CellKey((long long)floor(D.X * 512.0 + 0.5),
                                                                            (long long)floor(D.Y * 512.0 + 0.5),
                                                                            (long long)floor(D.Z * 512.0 + 0.5));
                        const long long H2 = RdxLevelRound::Detail::CellKey((long long)floor(P.X * 4.0 + 0.5),
                                                                            (long long)floor(P.Y * 4.0 + 0.5),
                                                                            (long long)floor(P.Z * 4.0 + 0.5));
                        return (long long)((unsigned long long)H1 * 0x9E3779B97F4A7C15ull ^ (unsigned long long)H2);
                    };
                    // Each side by its two places exactly, its polygons; and each line, the polygons with a side on it.
                    static std::map<std::pair<long long, long long>, std::vector<int>> Sides;
                    static std::unordered_map<long long, std::vector<int>>             OnLine;
                    if (Sides.empty())
                        for (int q = 0; q < NumPolys; q++)
                        {
                            const FPolyIn& P = Polys[q];
                            for (int k = 0; k < P.Count; k++)
                            {
                                const FV3&      A  = Pts[P.First + k];
                                const FV3&      B  = Pts[P.First + (k + 1) % P.Count];
                                const long long Ka = PlaceKey(A), Kb = PlaceKey(B);
                                Sides[{ (Ka < Kb) ? Ka : Kb, (Ka < Kb) ? Kb : Ka }].push_back(q);
                                OnLine[LineKey(A, B)].push_back(q);
                            }
                        }
                    std::vector<int> Runs;
                    for (int q = 0; q < NumPolys; q++)
                    {
                        if (!In[q])
                            continue;
                        const FPolyIn& P = Polys[q];
                        for (int k = 0; k < P.Count; k++)
                        {
                            const FV3&      A  = Pts[P.First + k];
                            const FV3&      B  = Pts[P.First + (k + 1) % P.Count];
                            const long long Ka = PlaceKey(A), Kb = PlaceKey(B);
                            const auto      It = Sides.find({ (Ka < Kb) ? Ka : Kb, (Ka < Kb) ? Kb : Ka });
                            bool            bHard = false;
                            if (It != Sides.end())
                                for (const int m : It->second)
                                    if (m != q && Polys[m].Nx * P.Nx + Polys[m].Ny * P.Ny + Polys[m].Nz * P.Nz < SoftCosR)
                                        bHard = true;
                            if (!bHard)
                                continue;
                            const auto Ln = OnLine.find(LineKey(A, B));
                            if (Ln == OnLine.end())
                                continue;
                            if (atoi(getenv("LRC_HALO_RUNS")) == 4)
                            {
                                // The places' sides, once: a place by its key, the polygons and corners of the sides from it.
                                static std::unordered_map<long long, std::vector<std::pair<int, int>>> SidesAt;
                                if (SidesAt.empty())
                                    for (int m = 0; m < NumPolys; m++)
                                        for (int j = 0; j < Polys[m].Count; j++)
                                        {
                                            SidesAt[PlaceKey(Pts[Polys[m].First + j])].push_back({ m, j });
                                            SidesAt[PlaceKey(Pts[Polys[m].First + (j + 1) % Polys[m].Count])].push_back({ m, j });
                                        }
                                auto Across = [&](int m, int j, int& o) {
                                    const FV3&      U  = Pts[Polys[m].First + j];
                                    const FV3&      W  = Pts[Polys[m].First + (j + 1) % Polys[m].Count];
                                    const long long Ku = PlaceKey(U), Kw = PlaceKey(W);
                                    const auto      Si = Sides.find({ (Ku < Kw) ? Ku : Kw, (Ku < Kw) ? Kw : Ku });
                                    o                  = -1;
                                    if (Si != Sides.end())
                                        for (const int c : Si->second)
                                            if (c != m && Polys[c].Nx * Polys[m].Nx + Polys[c].Ny * Polys[m].Ny + Polys[c].Nz * Polys[m].Nz < SoftCosR)
                                                o = c;
                                    return o >= 0;
                                };
                                int Oq = -1;
                                if (!Across(q, k, Oq))
                                    continue;
                                FV3 W0 = B - A;
                                W0     = W0 * (1.0 / RdxSphere::Length(W0));
                                const FV3 Na = RdxSphere::V3(P.Nx, P.Ny, P.Nz), Nb = RdxSphere::V3(Polys[Oq].Nx, Polys[Oq].Ny, Polys[Oq].Nz);
                                auto SamePlanes = [&](int m, int o) {
                                    const FV3 Ma = RdxSphere::V3(Polys[m].Nx, Polys[m].Ny, Polys[m].Nz);
                                    const FV3 Mb = RdxSphere::V3(Polys[o].Nx, Polys[o].Ny, Polys[o].Nz);
                                    return (RdxSphere::Dot(Ma, Na) >= 0.99999 && RdxSphere::Dot(Mb, Nb) >= 0.99999) ||
                                           (RdxSphere::Dot(Ma, Nb) >= 0.99999 && RdxSphere::Dot(Mb, Na) >= 0.99999);
                                };
                                std::set<std::pair<int, int>>    Seen4;
                                std::vector<std::pair<int, int>> Todo = { { q, k } };
                                Seen4.insert({ q, k });
                                while (!Todo.empty())
                                {
                                    const auto Cur = Todo.back();
                                    Todo.pop_back();
                                    int Oc = -1;
                                    Across(Cur.first, Cur.second, Oc);
                                    Runs.push_back(Cur.first);
                                    if (Oc >= 0)
                                        Runs.push_back(Oc);
                                    const FPolyIn& Pc = Polys[Cur.first];
                                    for (int e2 = 0; e2 < 2; e2++)
                                    {
                                        const FV3& X = Pts[Pc.First + (Cur.second + e2) % Pc.Count];
                                        const auto At2 = SidesAt.find(PlaceKey(X));
                                        if (At2 == SidesAt.end())
                                            continue;
                                        for (const auto& Nx2 : At2->second)
                                        {
                                            if (Seen4.count(Nx2))
                                                continue;
                                            const FPolyIn& Pm = Polys[Nx2.first];
                                            const FV3& U2 = Pts[Pm.First + Nx2.second];
                                            const FV3& V2 = Pts[Pm.First + (Nx2.second + 1) % Pm.Count];
                                            FV3        W2 = V2 - U2;
                                            const double L2 = RdxSphere::Length(W2);
                                            int          O2 = -1;
                                            if (!(L2 > 1e-6) || fabs(RdxSphere::Dot(W2 * (1.0 / L2), W0)) < 0.99999 ||
                                                !Across(Nx2.first, Nx2.second, O2) || !SamePlanes(Nx2.first, O2))
                                                continue;
                                            Seen4.insert(Nx2);
                                            Todo.push_back(Nx2);
                                        }
                                    }
                                }
                                continue;
                            }
                            if (atoi(getenv("LRC_HALO_RUNS")) != 3)
                            {
                                for (const int m : Ln->second)
                                    Runs.push_back(m);
                                continue;
                            }
                            // The line's sides along it (by this side's way): the hard ones chained from this one.
                            FV3          Dir = B - A;
                            const double Dl  = RdxSphere::Length(Dir);
                            if (!(Dl > 1e-6))
                                continue;
                            Dir = Dir * (1.0 / Dl);
                            struct FSeg
                            {
                                double T0, T1;
                                int    Poly;
                                bool   bHard;
                            };
                            std::vector<FSeg> Segs;
                            for (const int m : Ln->second)
                            {
                                const FPolyIn& Pm = Polys[m];
                                for (int j = 0; j < Pm.Count; j++)
                                {
                                    const FV3& U = Pts[Pm.First + j];
                                    const FV3& W = Pts[Pm.First + (j + 1) % Pm.Count];
                                    if (LineKey(U, W) != Ln->first)
                                        continue;
                                    const long long Ku = PlaceKey(U), Kw = PlaceKey(W);
                                    const auto      Si = Sides.find({ (Ku < Kw) ? Ku : Kw, (Ku < Kw) ? Kw : Ku });
                                    bool            bH = false;
                                    if (Si != Sides.end())
                                        for (const int o : Si->second)
                                            if (o != m && Polys[o].Nx * Pm.Nx + Polys[o].Ny * Pm.Ny + Polys[o].Nz * Pm.Nz < SoftCosR)
                                                bH = true;
                                    const double Tu = RdxSphere::Dot(U, Dir), Tw = RdxSphere::Dot(W, Dir);
                                    Segs.push_back({ (Tu < Tw) ? Tu : Tw, (Tu < Tw) ? Tw : Tu, m, bH });
                                }
                            }
                            const double Ta = RdxSphere::Dot(A, Dir), Tb = RdxSphere::Dot(B, Dir);
                            double       R0 = (Ta < Tb) ? Ta : Tb, R1 = (Ta < Tb) ? Tb : Ta;
                            for (bool bGrew = true; bGrew;)
                            {
                                bGrew = false;
                                for (const FSeg& G : Segs)
                                    if (G.bHard && G.T0 <= R1 + 0.5 && G.T1 >= R0 - 0.5 && (G.T0 < R0 || G.T1 > R1))
                                    {
                                        R0    = (G.T0 < R0) ? G.T0 : R0;
                                        R1    = (G.T1 > R1) ? G.T1 : R1;
                                        bGrew = true;
                                    }
                            }
                            for (const FSeg& G : Segs)
                                if (G.T0 <= R1 + 0.5 && G.T1 >= R0 - 0.5)
                                    Runs.push_back(G.Poly);
                        }
                    }
                    for (const int m : Runs)
                        In[m] = 1;
                };
                if (getenv("LRC_HALO_RUNS"))
                    RunsOnce();
                // LRC_HALO_NEAR=<units>: and every polygon whose box comes within that of a polygon taken so far - the
                // still ones a taken place's move may be held behind (LevelRound.h's HoldBehind).
                if (const char* NearText = getenv("LRC_HALO_NEAR"))
                {
                    const double Near = atof(NearText), Cell = 64.0;
                    std::unordered_map<long long, std::vector<int>> Boxes;
                    for (int q = 0; q < NumPolys; q++)
                        if (In[q])
                            for (long long x = (long long)floor((Lo[q].X - Near) / Cell); x <= (long long)floor((Hi[q].X + Near) / Cell); x++)
                                for (long long y = (long long)floor((Lo[q].Y - Near) / Cell); y <= (long long)floor((Hi[q].Y + Near) / Cell); y++)
                                    for (long long z = (long long)floor((Lo[q].Z - Near) / Cell); z <= (long long)floor((Hi[q].Z + Near) / Cell); z++)
                                        Boxes[RdxLevelRound::Detail::CellKey(x, y, z)].push_back(q);
                    std::vector<int> Add;
                    for (int q = 0; q < NumPolys; q++)
                    {
                        if (In[q])
                            continue;
                        bool bNear = false;
                        for (long long x = (long long)floor(Lo[q].X / Cell); x <= (long long)floor(Hi[q].X / Cell) && !bNear; x++)
                            for (long long y = (long long)floor(Lo[q].Y / Cell); y <= (long long)floor(Hi[q].Y / Cell) && !bNear; y++)
                                for (long long z = (long long)floor(Lo[q].Z / Cell); z <= (long long)floor(Hi[q].Z / Cell) && !bNear; z++)
                                {
                                    const auto It = Boxes.find(RdxLevelRound::Detail::CellKey(x, y, z));
                                    if (It == Boxes.end())
                                        continue;
                                    for (const int m : It->second)
                                        if (Lo[q].X <= Hi[m].X + Near && Hi[q].X >= Lo[m].X - Near && Lo[q].Y <= Hi[m].Y + Near &&
                                            Hi[q].Y >= Lo[m].Y - Near && Lo[q].Z <= Hi[m].Z + Near && Hi[q].Z >= Lo[m].Z - Near)
                                        {
                                            bNear = true;
                                            break;
                                        }
                                }
                        if (bNear)
                            Add.push_back(q);
                    }
                    for (const int q : Add)
                        In[q] = 1;
                }
                if (getenv("LRC_HALO_RUNS") && atoi(getenv("LRC_HALO_RUNS")) >= 2)
                    RunsOnce();
            }
            if (bRegionIndex)
            {
                TList<int>   IOwn, IOut;
                const double Tp = Now();
                if (!RdxLevelRound::RegionPolysOf(RegionIndex, Pts.Data, Polys.Data, CubeLo, CellSize, IOwn, IOut))
                {
                    printf("region %d: no memory for its polygons\n", c);
                    continue;
                }
                std::vector<unsigned char> Mine(NumPolys, 0);
                for (int i = 0; i < IOut.Num; i++)
                    Mine[IOut[i]] = 1;
                int Missing = 0, Extra = 0;
                for (int q = 0; q < NumPolys; q++)
                {
                    const bool bHere = In[q] || !(Hi[q].X < BoxLo.X || Lo[q].X > BoxHi.X || Hi[q].Y < BoxLo.Y ||
                                                  Lo[q].Y > BoxHi.Y || Hi[q].Z < BoxLo.Z || Lo[q].Z > BoxHi.Z);
                    Missing += (bHere && !Mine[q]) ? 1 : 0;
                    Extra += (!bHere && Mine[q]) ? 1 : 0;
                }
                printf("  the index: its own %d (here %d), its work %d polygons; against the ones made here %d missing, %d more\n",
                       IOwn.Num, (int)Own.size(), IOut.Num, Missing, Extra);
                printf("  the index: the work's polygons found in %.1f ms\n", (Now() - Tp) * 1000.0);
                for (int q = 0; q < NumPolys; q++)
                    In[q] = Mine[q];
            }
            for (int q = 0; q < NumPolys; q++)
            {
                if (!In[q] && (Hi[q].X < BoxLo.X || Lo[q].X > BoxHi.X || Hi[q].Y < BoxLo.Y || Lo[q].Y > BoxHi.Y ||
                               Hi[q].Z < BoxLo.Z || Lo[q].Z > BoxHi.Z))
                    continue;
                FPolyIn P = Polys[q];
                P.First   = SubPts.Num;
                for (int k = 0; k < Polys[q].Count; k++)
                    SubPts.Add(Pts[Polys[q].First + k]);
                SubOf[q] = SubPolys.Num;
                SubPolys.Add(P);
                Orig.push_back(q);
            }
            FResult Rs;
            Rs.Workers = R.Workers;
            Rs.Sag     = R.Sag;
            const double Tc = Now();
            RdxLevelRound::FBevelStats Sr;
            RdxLevelRound::FBevelMade  Br;
            if (!RdxLevelRound::Round(SubPts.Data, SubPts.Num, SubPolys.Data, SubPolys.Num, Cap, Angle, Rs) ||
                (bRegionBevel && !RdxLevelRound::Bevel(SubPts.Data, SubPts.Num, SubPolys.Data, SubPolys.Num, nullptr,
                                                       nullptr, 0, Angle, 1.0, Rs, Sr, Br)))
            {
                printf("region %d: no memory\n", c);
                continue;
            }
            const double Mc = (Now() - Tc) * 1000.0;
            int    Other = 0, Off = 0, Moved = 0;
            double Worst = 0.0;
            int    WorstPoly = -1;
            for (const int q : Own)
            {
                const int s2 = SubOf[q];
                const int Nw = (q < R.PolyTris.Num) ? R.PolyTris[q] : 0;
                const int Ns = (s2 >= 0 && s2 < Rs.PolyTris.Num) ? Rs.PolyTris[s2] : 0;
                if (Nw > 0)
                    Moved++;
                if (Nw != Ns)
                {
                    Other++;
                    if (getenv("LRC_REGION_SAY") && bFlatHalo)
                    {
                        int Members = 0, Taken = 0;
                        for (int m = 0; m < NumPolys; m++)
                            if (FlatOf[m] == FlatOf[q])
                            {
                                Members++;
                                Taken += (SubOf[m] >= 0) ? 1 : 0;
                            }
                        printf("  other: polygon %d (%d triangles, alone %d), its flat polygon %d %s, %d polygons, %d of "
                               "them taken\n", q, Nw, Ns, FlatOf[q], FlatSoft[FlatOf[q]] ? "soft" : "hard", Members, Taken);
                    }
                    continue;
                }
                double Far = 0.0;
                for (int i = 0; i < Ns * 3; i++)
                {
                    const FV3& X    = Rs.Moved[Rs.Tri[Rs.PolyFirst[s2] * 3 + i]];
                    double     Best = 1e30;
                    for (int j = 0; j < Nw * 3; j++)
                        Best = RdxSphere::MinOf(Best, RdxSphere::Length(R.Moved[R.Tri[R.PolyFirst[q] * 3 + j]] - X));
                    Far = RdxSphere::MaxOf(Far, Best);
                }
                if (Far > 1e-4)
                    Off++;
                // LRC_REGION_SAY=1: each polygon off, with its flat polygon - soft or not, how many polygons it has and
                // how many of them the round took.
                if (Far > 1e-4 && getenv("LRC_REGION_SAY") && bFlatHalo)
                {
                    int Members = 0, Taken = 0;
                    for (int m = 0; m < NumPolys; m++)
                        if (FlatOf[m] == FlatOf[q])
                        {
                            Members++;
                            Taken += (SubOf[m] >= 0) ? 1 : 0;
                        }
                    printf("  off %.4f: polygon %d (%d triangles), its flat polygon %d %s, %d polygons, %d of them taken\n",
                           Far, q, Ns, FlatOf[q], FlatSoft[FlatOf[q]] ? "soft" : "hard", Members, Taken);
                }
                if (Far > Worst)
                {
                    Worst     = Far;
                    WorstPoly = q;
                }
            }
            printf("region %d: %d polygons of its own (%d moved), %d with the round - %.0f ms, %.1f MB held; other "
                   "triangles %d, corners off by over 0.0001 %d, the most %.6f (polygon %d)\n", c, (int)Own.size(), Moved,
                   SubPolys.Num, Mc, (double)Rs.Bytes() / 1048576.0, Other, Off, Worst, WorstPoly);
            {
                PROCESS_MEMORY_COUNTERS Pm = {};
                Pm.cb                      = sizeof(Pm);
                if (GetProcessMemoryInfo(GetCurrentProcess(), &Pm, sizeof(Pm)))
                    printf("  the process: %.0f MB now, %.0f MB at the most so far\n", Pm.PagefileUsage / 1048576.0,
                           Pm.PeakPagefileUsage / 1048576.0);
            }
        }
        return 0;
    }
    // LRC_PLACES=<w>,<w>...: those places of the rounding's, where they were and where they went, before any bevel.
    if (const char* Look = getenv("LRC_PLACES"))
        for (const char* C = Look; *C;)
        {
            const int w = atoi(C);
            while (*C && *C != ',')
                C++;
            if (*C == ',')
                C++;
            if (w >= 0 && w < R.Orig.Num)
            {
                printf("place %d: was %.4f %.4f %.4f, now %.4f %.4f %.4f (moved %.4f); its polygons:", w, R.Orig[w].X,
                       R.Orig[w].Y, R.Orig[w].Z, R.Moved[w].X, R.Moved[w].Y, R.Moved[w].Z,
                       RdxSphere::Length(R.Moved[w] - R.Orig[w]));
                for (int p = 0; p < NumPolys && p < R.PolyTris.Num; p++)
                    for (int t = R.PolyFirst[p] * 3; t < (R.PolyFirst[p] + R.PolyTris[p]) * 3; t++)
                        if (R.Tri[t] == w)
                        {
                            printf(" %d (normal %.2f %.2f %.2f)", p, Polys[p].Nx, Polys[p].Ny, Polys[p].Nz);
                            break;
                        }
                printf("\n");
            }
        }
    // LRC_POLYM0=<p>,<p>...: as LRC_POLYM, the rounding's triangles before any bevel.
    if (const char* Look = getenv("LRC_POLYM0"))
        for (const char* C = Look; *C;)
        {
            const int p = atoi(C);
            while (*C && *C != ',')
                C++;
            if (*C == ',')
                C++;
            if (p < 0 || p >= NumPolys || p >= R.PolyTris.Num)
                continue;
            printf("polygon %d: normal %.4f %.4f %.4f w %.3f, use %d, %d triangles\n", p, Polys[p].Nx, Polys[p].Ny,
                   Polys[p].Nz, Polys[p].W, Polys[p].Use, R.PolyTris[p]);
            for (int t = R.PolyFirst[p] * 3; t < (R.PolyFirst[p] + R.PolyTris[p]) * 3; t += 3)
            {
                printf("  tri");
                for (int k = 0; k < 3; k++)
                {
                    const int  w = R.Tri[t + k];
                    const FV3& Q = R.Moved[w];
                    printf(" [%d %.3f %.3f %.3f b%.3f]", w, Q.X, Q.Y, Q.Z, 0.0);
                }
                printf("\n");
            }
        }
    // LRC_BEVEL=1 (any mode but "bevel", which makes it itself): the fine bevel on the rounding and the relief's outlines
    // read again after it, as the renderer does (SphereBevelOn) - every check below is then of the bevelled level.
    if (getenv("LRC_BEVEL") && !(Argc > 3 && strcmp(Argv[3], "bevel") == 0))
    {
        RdxLevelRound::FBevelStats S;
        RdxLevelRound::FBevelMade  Bm;
        bool bBevel = RdxLevelRound::Bevel(Pts.Data, NumPts, Polys.Data, NumPolys, nullptr, nullptr, 0, Angle, 1.0, R, S,
                                           Bm);
        if (bBevel && Bm.Polygons > 0 && !RdxLevelRound::ChordsOf(Pts.Data, NumPts, Polys.Data, NumPolys, R))
            bBevel = false;
        printf("bevelled first: %s, %d edges, %d polygons cut; its parts, ms:", bBevel ? "made" : "NO MEMORY", S.Edges,
               Bm.Polygons);
        for (int p = 0; p < RdxLevelRound::FBevelMade::Parts; p++)
            printf(" %.0f", Bm.PartMs[p]);
        printf("\n");
    }

    // Over the cap - and held at it: a place whose move came to the cap was cut down to it.
    int Over = 0, AtCap = 0, Moving = 0;
    for (int i = 0; i < R.Orig.Num; i++)
    {
        const double M = RdxSphere::Length(R.Moved[i] - R.Orig[i]);
        Over += (M > Cap + 1e-4) ? 1 : 0;
        AtCap += (M > Cap - 1e-3) ? 1 : 0;
        Moving += (M > 0.05) ? 1 : 0;
    }
    printf("places moving more than a twentieth of a unit: %d; held at the cap: %d; edges unbent %d, faces zipped %d; "
           "sheets held %d\n", Moving, AtCap, R.Unbent, R.Zipped, GSheetsHeld);
    printf("held behind still polygons: %d stopped, %d eased\n", R.HeldStopped, R.HeldEased);
    // With "lookup" after the angle: a point laid on a moved polygon (MoveOnPolygon) moved as the faces' planes and the
    // polygons' boxes made with the faces have it, set against the same lookup with each face's plane made where it is
    // asked and no box - to the bit, the found or not and the move. The points: each face's corners and middle, those
    // pushed off its plane by a little less and a little more than the reach and further, past its edges by a little,
    // and some at random in the polygon's box widened by twelve units.
    if (Argc > 3 && strcmp(Argv[3], "lookup") == 0)
    {
        auto Plain = [&](int p, const FV3& P, double Near, FV3& Move) {
            if (p < 0 || p >= R.PolyFaceCount.Num || R.PolyFaceCount[p] <= 0)
                return false;
            for (int f = R.PolyFaceFirst[p]; f < R.PolyFaceFirst[p] + R.PolyFaceCount[p]; f++)
            {
                float S[3] = { 0.0f, 0.0f, 0.0f };
                if (!RdxLevelRound::FaceSharesAt(R.FaceKind[f], &R.FaceCorner[f * 4], P, Near, S))
                    continue;
                RdxMeshRound::FEdgeTerms T[4];
                RdxLevelRound::FaceTermsOf(&R.FaceTerm[f * RdxLevelRound::FaceTermFloats], T);
                RdxLevelRound::FaceMoveFrom(R.FaceKind[f], T, S, R.MoveCap, Move);
                return true;
            }
            return false;
        };
        long long    Probes = 0, Found = 0, Differ = 0;
        // The points again, for the lookups as made to be timed on their own, in one go, after the check.
        TList<int>   TimedPoly;
        TList<FV3>   TimedAt;
        unsigned int Seed   = 12345u;
        auto         Next   = [&]() {
            Seed = Seed * 1664525u + 1013904223u;
            return (double)(Seed >> 8) / 16777216.0;
        };
        auto Probe = [&](int p, const FV3& P) {
            const FV3  Sentinel = RdxSphere::V3(1234.5, -678.25, 9.125);
            FV3        A = Sentinel, B = Sentinel;
            const bool bA = Plain(p, P, 2.0, A);
            const bool bB = RdxLevelRound::MoveOnPolygon(R, p, P, 2.0, B);
            TimedPoly.Add(p);
            TimedAt.Add(P);
            Probes++;
            Found += bA ? 1 : 0;
            if (bA != bB || memcmp(&A, &B, sizeof(FV3)) != 0)
            {
                if (Differ < 8)
                    printf("lookup differs: polygon %d at (%.4f %.4f %.4f): plain %d (%.9g %.9g %.9g), made %d "
                           "(%.9g %.9g %.9g)\n", p, P.X, P.Y, P.Z, bA ? 1 : 0, A.X, A.Y, A.Z, bB ? 1 : 0, B.X, B.Y,
                           B.Z);
                Differ++;
            }
        };
        static const double Offs[] = { 0.0, 0.5, -0.5, 1.5, -1.5, 1.99, -1.99, 2.01, -2.01, 2.5, -2.5, 4.0, -4.0, 5.9,
                                       -5.9, 6.1, -6.1, 8.0, -8.0 };
        static const double Outs[] = { 1.0005, 1.002, 1.01, 1.05, 1.2 };
        for (int p = 0; p < NumPolys && p < R.PolyFaceCount.Num; p++)
        {
            if (R.PolyFaceCount[p] <= 0)
                continue;
            for (int f = R.PolyFaceFirst[p]; f < R.PolyFaceFirst[p] + R.PolyFaceCount[p]; f++)
            {
                const FV3* const Q     = &R.FaceCorner[f * 4];
                const int        Sides = (R.FaceKind[f] == 0) ? 3 : 4;
                FV3              Mid   = RdxSphere::V3(0.0, 0.0, 0.0);
                for (int k = 0; k < Sides; k++)
                    Mid = Mid + Q[k] * (1.0 / Sides);
                const FV3    N  = (R.FaceKind[f] == 0) ? RdxSphere::Cross(Q[1] - Q[0], Q[2] - Q[0])
                                                       : RdxSphere::Cross(Q[1] - Q[0], Q[3] - Q[0]);
                const double NL = RdxSphere::Length(N);
                const FV3    Up = (NL > 1e-12) ? N * (1.0 / NL) : RdxSphere::V3(0.0, 0.0, 1.0);
                for (const double O : Offs)
                    Probe(p, Mid + Up * O);
                for (int k = 0; k < Sides; k++)
                {
                    Probe(p, Q[k]);
                    Probe(p, Q[k] + Up * 1.99);
                    Probe(p, Q[k] - Up * 2.01);
                    for (const double S : Outs)
                    {
                        const FV3 Past = Mid + (Q[k] - Mid) * S;
                        Probe(p, Past);
                        Probe(p, Past + Up * 1.0);
                        Probe(p, Past - Up * 1.9);
                    }
                }
            }
            const double* const Bx = &R.PolyBox[p * 6];
            for (int i = 0; i < 16; i++)
                Probe(p, RdxSphere::V3(Bx[0] - 12.0 + Next() * (Bx[3] - Bx[0] + 24.0),
                                       Bx[1] - 12.0 + Next() * (Bx[4] - Bx[1] + 24.0),
                                       Bx[2] - 12.0 + Next() * (Bx[5] - Bx[2] + 24.0)));
        }
        LARGE_INTEGER TickRate, LookFrom, LookTo;
        QueryPerformanceFrequency(&TickRate);
        QueryPerformanceCounter(&LookFrom);
        double Sum = 0.0;
        for (int i = 0; i < TimedPoly.Num && i < TimedAt.Num; i++)
        {
            FV3 M = RdxSphere::V3(0.0, 0.0, 0.0);
            if (RdxLevelRound::MoveOnPolygon(R, TimedPoly[i], TimedAt[i], 2.0, M))
                Sum += 1.0 + M.X + M.Y + M.Z;
        }
        QueryPerformanceCounter(&LookTo);
        printf("lookup check: %lld points, %lld on a face, %lld differ; the lookups as made %.1f ms (sum %.6f)\n",
               Probes, Found, Differ,
               1000.0 * (double)(LookTo.QuadPart - LookFrom.QuadPart) / (double)(TickRate.QuadPart > 0 ? TickRate.QuadPart : 1),
               Sum);
        return Differ ? 1 : 0;
    }
    // With "bevelcount" after the angle: what the fine bevel (EdgeBevel.h, radius 1) would cut the level into - each
    // polygon the round leaves where it is, along its outward hard edges (sharper than the angle) to another such polygon
    // that shares the side end to end, cut by CutBands; the triangles a polygon is drawn with, before and after.
    if (Argc > 3 && strcmp(Argv[3], "bevelcount") == 0)
    {
        const double Pi = 3.14159265358979;
        auto Quant = [](const FV3& V) {
            return std::make_tuple((long long)floor(V.X * 64.0 + 0.5), (long long)floor(V.Y * 64.0 + 0.5),
                                   (long long)floor(V.Z * 64.0 + 0.5));
        };
        auto Still = [&](int p) {
            const FPolyIn& P = Polys[p];
            return P.Count >= 3 && (P.Use & RdxSphere::Use_Wall) && !(P.Use & RdxSphere::Use_Sheet) &&
                   (p >= R.PolyTris.Num || R.PolyTris[p] == 0);
        };
        std::map<std::tuple<long long, long long, long long, long long, long long, long long>, int> Sides;
        for (int p = 0; p < NumPolys; p++)
            if (Still(p))
                for (int k = 0; k < Polys[p].Count; k++)
                {
                    const auto A = Quant(Pts[Polys[p].First + k]), B = Quant(Pts[Polys[p].First + (k + 1) % Polys[p].Count]);
                    Sides[std::tuple_cat(A, B)] = p;
                }
        long long Before = 0, After = 0, Cut = 0, CutEdges = 0, Most = 0;
        TList<FV3>             Corners, Points;
        TList<int>             Pieces;
        TList<RdxBevel::FEdge> Edges;
        for (int p = 0; p < NumPolys; p++)
        {
            if (!Still(p))
                continue;
            const FPolyIn& P = Polys[p];
            const FV3      N = RdxSphere::V3(P.Nx, P.Ny, P.Nz);
            FV3            Mid = RdxSphere::V3(0.0, 0.0, 0.0);
            Corners.Clear();
            for (int k = 0; k < P.Count; k++)
            {
                Corners.Add(Pts[P.First + k]);
                Mid = Mid + Pts[P.First + k] * (1.0 / P.Count);
            }
            Before += P.Count - 2;
            Edges.Clear();
            for (int k = 0; k < P.Count; k++)
            {
                const FV3& A = Corners[k];
                const FV3& B = Corners[(k + 1) % P.Count];
                const auto It = Sides.find(std::tuple_cat(Quant(B), Quant(A)));
                if (It == Sides.end() || It->second == p)
                    continue;
                const FPolyIn&  Q = Polys[It->second];
                RdxBevel::FEdge E;
                if (RdxBevel::MakeEdge(A, B, N, RdxSphere::V3(Q.Nx, Q.Ny, Q.Nz), Mid, Pi - Angle * Pi / 180.0,
                                       175.0 * Pi / 180.0, E))
                    Edges.Add(E);
            }
            if (Edges.Num == 0 || !RdxBevel::CutBands(Corners.Data, Corners.Num, N, Edges.Data, Edges.Num, 1.0, Points,
                                                      Pieces))
            {
                After += P.Count - 2;
                continue;
            }
            long long Tris = 0;
            for (int q = 0; q < Pieces.Num; q += Pieces[q] + 1)
                Tris += Pieces[q] - 2;
            After += Tris;
            Cut++;
            CutEdges += Edges.Num;
            Most = (Tris > Most) ? Tris : Most;
        }
        printf("bevel (radius 1): %lld polygons cut along %lld edges; the still polygons' triangles %lld before, %lld after "
               "(+%lld; the most one polygon becomes %lld)\n", Cut, CutEdges, Before, After, After - Before, Most);
    }
    // With "bevelkinds" after the angle: the outward hard ring sides between two walls, by what the rounding did to their
    // two polygons - still (the bevel's own), moved along their plane only, moved off it - and whether the side's own
    // places moved: how many, and how long they are together.
    if (Argc > 3 && strcmp(Argv[3], "bevelkinds") == 0)
    {
        const double Pi        = 3.14159265358979;
        const double LeastTurn = Pi - Angle * Pi / 180.0, MostTurn = 175.0 * Pi / 180.0;
        std::vector<int> Kind(NumPolys, 3);
        for (int p = 0; p < NumPolys && p < R.RingCount.Num; p++)
        {
            const FPolyIn& P = Polys[p];
            if (!(P.Use & RdxSphere::Use_Wall) || (P.Use & RdxSphere::Use_Sheet) || R.RingCount[p] < 3)
                continue;
            if (p >= R.PolyTris.Num || R.PolyTris[p] == 0)
            {
                Kind[p] = 0;
                continue;
            }
            bool bFlat = true;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p] && bFlat; t++)
                for (int k = 0; k < 3 && bFlat; k++)
                {
                    const FV3& M = R.Moved[R.Tri[t * 3 + k]];
                    bFlat        = fabs(M.X * P.Nx + M.Y * P.Ny + M.Z * P.Nz - P.W) <= 0.01;
                }
            Kind[p] = bFlat ? 1 : 2;
        }
        for (int k = 0; k < R.Left.Num; k++)
            if (R.Left[k] >= 0 && R.Left[k] < NumPolys)
                Kind[R.Left[k]] = 3;
        std::map<std::pair<int, int>, int> SideOf;
        for (int p = 0; p < NumPolys && p < R.RingCount.Num; p++)
            if (Kind[p] != 3)
                for (int i = 0; i < R.RingCount[p]; i++)
                    SideOf[{ R.Ring[R.RingFirst[p] + i], R.Ring[R.RingFirst[p] + (i + 1) % R.RingCount[p]] }] = p;
        long long Count[3][3][2] = {};
        double    Len[3][3][2]   = {};
        for (const auto& It : SideOf)
        {
            const int U = It.first.first, V = It.first.second, p = It.second;
            if (U >= V)
                continue;
            const auto Back = SideOf.find({ V, U });
            if (Back == SideOf.end() || Back->second == p)
                continue;
            const int      q = Back->second;
            const FPolyIn &A = Polys[p], &B = Polys[q];
            const FV3      Na = RdxSphere::V3(A.Nx, A.Ny, A.Nz), Nb = RdxSphere::V3(B.Nx, B.Ny, B.Nz);
            const double   Turn = acos(fmax(-1.0, fmin(1.0, RdxSphere::Dot(Na, Nb))));
            if (!(Turn > LeastTurn && Turn < MostTurn))
                continue;
            FV3 Mid = RdxSphere::V3(0.0, 0.0, 0.0);
            for (int i = 0; i < R.RingCount[p]; i++)
                Mid = Mid + R.Orig[R.Ring[R.RingFirst[p] + i]] * (1.0 / R.RingCount[p]);
            const FV3 Way  = RdxBevel::Unit(R.Orig[V] - R.Orig[U]);
            FV3       Into = RdxBevel::Unit(RdxSphere::Cross(Na, Way));
            if (RdxSphere::Dot(Into, Mid - R.Orig[U]) < 0.0)
                Into = Into * -1.0;
            if (RdxSphere::Dot(Into, Nb) >= 0.0)
                continue;
            const int Ka = (Kind[p] < Kind[q]) ? Kind[p] : Kind[q], Kb = (Kind[p] < Kind[q]) ? Kind[q] : Kind[p];
            const int Mv = (RdxSphere::Length(R.Moved[U] - R.Orig[U]) > 0.01 ||
                            RdxSphere::Length(R.Moved[V] - R.Orig[V]) > 0.01) ? 1 : 0;
            Count[Ka][Kb][Mv]++;
            Len[Ka][Kb][Mv] += RdxSphere::Length(R.Orig[V] - R.Orig[U]);
        }
        long long StillPolys = 0, MovedTris = 0;
        for (int p = 0; p < NumPolys; p++)
        {
            StillPolys += (Kind[p] == 0) ? 1 : 0;
            MovedTris += (Kind[p] == 1 || Kind[p] == 2) ? R.PolyTris[p] : 0;
        }
        // The moved walls with a ring side to another wall turning by more than 35 degrees (the rounding's least hard
        // turn less forty): those whose triangles a bevel on the rounding's surface would look at.
        std::vector<unsigned char> Hot(NumPolys, 0);
        for (const auto& It : SideOf)
        {
            const auto Back = SideOf.find({ It.first.second, It.first.first });
            if (Back == SideOf.end() || Back->second == It.second)
                continue;
            const FPolyIn &A = Polys[It.second], &B = Polys[Back->second];
            if (A.Nx * B.Nx + A.Ny * B.Ny + A.Nz * B.Nz < cos(35.0 * Pi / 180.0))
                Hot[It.second] = 1;
        }
        long long HotTris = 0;
        for (int p = 0; p < NumPolys; p++)
            HotTris += (Hot[p] && (Kind[p] == 1 || Kind[p] == 2)) ? R.PolyTris[p] : 0;
        // And of all moved walls' triangles, those whose box comes within 24 units of an outward ring side turning by
        // more than 35 degrees - the widest band (a turn short of a knife's) and a unit.
        std::map<std::tuple<long long, long long, long long>, std::vector<int>> Near;
        std::vector<FV3> NearLo, NearHi;
        for (const auto& It : SideOf)
        {
            const int U = It.first.first, V = It.first.second, p = It.second;
            if (U >= V)
                continue;
            const auto Back = SideOf.find({ V, U });
            if (Back == SideOf.end() || Back->second == p)
                continue;
            const FPolyIn &A = Polys[p], &B = Polys[Back->second];
            const FV3      Na = RdxSphere::V3(A.Nx, A.Ny, A.Nz), Nb = RdxSphere::V3(B.Nx, B.Ny, B.Nz);
            if (RdxSphere::Dot(Na, Nb) >= cos(35.0 * Pi / 180.0))
                continue;
            FV3 Mid = RdxSphere::V3(0.0, 0.0, 0.0);
            for (int i = 0; i < R.RingCount[p]; i++)
                Mid = Mid + R.Orig[R.Ring[R.RingFirst[p] + i]] * (1.0 / R.RingCount[p]);
            const FV3 Way  = RdxBevel::Unit(R.Orig[V] - R.Orig[U]);
            FV3       Into = RdxBevel::Unit(RdxSphere::Cross(Na, Way));
            if (RdxSphere::Dot(Into, Mid - R.Orig[U]) < 0.0)
                Into = Into * -1.0;
            if (RdxSphere::Dot(Into, Nb) >= 0.0)
                continue;
            const FV3&   Pa    = R.Moved[U];
            const FV3&   Pb    = R.Moved[V];
            const int    s     = (int)NearLo.size();
            const double Turn  = acos(fmax(-1.0, fmin(1.0, RdxSphere::Dot(Na, Nb))));
            const double Reach = (getenv("LRC_REACH24") ? 24.0 : tan(0.5 * fmin(Turn + 40.0 * Pi / 180.0, 175.0 * Pi / 180.0)) + 1.0);
            NearLo.push_back(RdxSphere::V3(fmin(Pa.X, Pb.X) - Reach, fmin(Pa.Y, Pb.Y) - Reach, fmin(Pa.Z, Pb.Z) - Reach));
            NearHi.push_back(RdxSphere::V3(fmax(Pa.X, Pb.X) + Reach, fmax(Pa.Y, Pb.Y) + Reach, fmax(Pa.Z, Pb.Z) + Reach));
            for (long long z = (long long)floor(NearLo[s].Z / 64.0); z <= (long long)floor(NearHi[s].Z / 64.0); z++)
                for (long long y = (long long)floor(NearLo[s].Y / 64.0); y <= (long long)floor(NearHi[s].Y / 64.0); y++)
                    for (long long x = (long long)floor(NearLo[s].X / 64.0); x <= (long long)floor(NearHi[s].X / 64.0);
                         x++)
                        Near[{ x, y, z }].push_back(s);
        }
        long long NearTris = 0;
        for (int p = 0; p < NumPolys; p++)
        {
            if (Kind[p] != 1 && Kind[p] != 2)
                continue;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                FV3 Lo = R.Moved[R.Tri[t * 3]], Hi = Lo;
                for (int k = 1; k < 3; k++)
                {
                    const FV3& Q = R.Moved[R.Tri[t * 3 + k]];
                    Lo = RdxSphere::V3(fmin(Lo.X, Q.X), fmin(Lo.Y, Q.Y), fmin(Lo.Z, Q.Z));
                    Hi = RdxSphere::V3(fmax(Hi.X, Q.X), fmax(Hi.Y, Q.Y), fmax(Hi.Z, Q.Z));
                }
                bool bNear = false;
                for (long long z = (long long)floor(Lo.Z / 64.0); z <= (long long)floor(Hi.Z / 64.0) && !bNear; z++)
                    for (long long y = (long long)floor(Lo.Y / 64.0); y <= (long long)floor(Hi.Y / 64.0) && !bNear; y++)
                        for (long long x = (long long)floor(Lo.X / 64.0); x <= (long long)floor(Hi.X / 64.0) && !bNear;
                             x++)
                        {
                            const auto C = Near.find({ x, y, z });
                            if (C == Near.end())
                                continue;
                            for (const int s : C->second)
                                if (Lo.X <= NearHi[s].X && Hi.X >= NearLo[s].X && Lo.Y <= NearHi[s].Y &&
                                    Hi.Y >= NearLo[s].Y && Lo.Z <= NearHi[s].Z && Hi.Z >= NearLo[s].Z)
                                {
                                    bNear = true;
                                    break;
                                }
                        }
                NearTris += bNear ? 1 : 0;
            }
        }
        printf("bevel sizes: %d places, %lld still polygons, %lld triangles of moved walls, %lld of them of walls with a "
               "hard side, %lld near an outward one\n", R.Orig.Num, StillPolys, MovedTris, HotTris, NearTris);
        const char* Names[3] = { "still", "along its plane", "off its plane" };
        for (int a = 0; a < 3; a++)
            for (int b = a; b < 3; b++)
                printf("bevel kinds: %s / %s: %lld sides (%.0f units) whose places stayed, %lld (%.0f units) whose places "
                       "moved\n", Names[a], Names[b], Count[a][b][0], Len[a][b][0], Count[a][b][1], Len[a][b][1]);
    }
    // With "bevel" after the angle: the fine bevel (LevelBevel.h, radius 1) made on the rounding - its edges, those left
    // sharp and why, what it cut, the time it took - and checked: each triangle side of a polygon it cut has the same
    // side the other way round in another triangle, or is a side of a polygon it left alone, whose places did not move;
    // anything else opens the surface.
    if (Argc > 3 && strcmp(Argv[3], "bevel") == 0)
    {
        TList<int> TrisWere;
        if (!TrisWere.Resize(R.PolyTris.Num))
            return 1;
        for (int p = 0; p < R.PolyTris.Num; p++)
            TrisWere[p] = R.PolyTris[p];
        const int                  PlacesWere = R.Orig.Num;
        const std::vector<FV3>     MovedWere(R.Moved.Data, R.Moved.Data + R.Moved.Num);
        const std::vector<int>     TriWere(R.Tri.Data, R.Tri.Data + R.Tri.Num);
        const std::vector<int>     FirstWere(R.PolyFirst.Data, R.PolyFirst.Data + R.PolyFirst.Num);
        RdxLevelRound::FBevelStats S;
        RdxLevelRound::FBevelMade  Bm;
        const double               T1      = Now();
        const bool                 bBevel  = RdxLevelRound::Bevel(Pts.Data, NumPts, Polys.Data, NumPolys, nullptr,
                                                                     nullptr, 0, Angle, 1.0, R, S, Bm);
        const double               BevelMs = (Now() - T1) * 1000.0;
        // LRC_RENDER: the same eye's picture of what the bevel draws, beside the rounding's.
        if (bRender && bBevel)
        {
            char Name[600];
            snprintf(Name, sizeof(Name), "%s-bevelled.ppm", RenderPrefix);
            RenderResult(R, Pts.Data, Polys.Data, NumPolys, RenderCam, RenderW, RenderH, Name);
        }
        // LRC_BEVEL_MOVES="x0,y0,z0,x1,y1,z1,file": each place whose spot before the bevel's own move lies in that box -
        // its number, where it was before the bevel's move, where it is after, and whether the bevel made it (1) - one
        // line each.
        if (const char* MovesAsk = getenv("LRC_BEVEL_MOVES"))
        {
            double Box[6];
            char   Out[512] = {};
            if (bBevel && sscanf(MovesAsk, "%lf,%lf,%lf,%lf,%lf,%lf,%511s", &Box[0], &Box[1], &Box[2], &Box[3], &Box[4], &Box[5],
                                 Out) == 7)
                if (FILE* O = fopen(Out, "w"))
                {
                    const RdxSphere::TList<FV3>& Before = RdxLevelRound::BevelDetail::LevelBevelTraceBefore();
                    const int                    Had    = RdxLevelRound::BevelDetail::LevelBevelTracePlaces0();
                    int                          Said   = 0;
                    for (int w = 0; w < Before.Num && w < R.Moved.Num; w++)
                    {
                        const FV3& B = Before[w];
                        if (B.X < Box[0] || B.X > Box[3] || B.Y < Box[1] || B.Y > Box[4] || B.Z < Box[2] || B.Z > Box[5])
                            continue;
                        const FV3& A = R.Moved[w];
                        fprintf(O, "%d %.5f %.5f %.5f %.5f %.5f %.5f %d\n", w, B.X, B.Y, B.Z, A.X, A.Y, A.Z, w >= Had ? 1 : 0);
                        Said++;
                    }
                    fclose(O);
                    printf("bevel moves: %d places written to %s\n", Said, Out);
                }
        }
        // The memory the process takes after the bevel, and the most it took: the bevel's room shows in the second.
        {
            PROCESS_MEMORY_COUNTERS Mc = {};
            Mc.cb                      = sizeof(Mc);
            if (GetProcessMemoryInfo(GetCurrentProcess(), &Mc, sizeof(Mc)))
                printf("bevel memory: %.0f MB now, %.0f MB at the most; the result holds %.0f MB (places %.0f, triangles %.0f, turns %.0f)\n", Mc.PagefileUsage / 1048576.0,
                       Mc.PeakPagefileUsage / 1048576.0, (double)R.Bytes() / 1048576.0,
                       (double)(R.Orig.Room + R.Moved.Room) * sizeof(FV3) / 1048576.0,
                       (double)R.Tri.Room * sizeof(int) / 1048576.0, (double)R.TriTurn.Room * sizeof(float) / 1048576.0);
            if (GMemLaps > 0)
            {
                printf("memory at the clock, the rounding's and then the bevel's (MB):");
                for (int i = 0; i < GMemLaps; i++)
                    printf(" %.0f", GMemLapMB[i]);
                printf("\n");
            }
        }
        // LRC_RBYTES=1: what the result holds after the bevel, list by list (MB) - what stays on the processor.
        if (getenv("LRC_RBYTES"))
        {
#define LRC_SAY_BYTES(L) printf(" " #L " %.1f", (double)FResult::BytesOf(R.L) / 1048576.0)
            printf("result lists (MB):");
            LRC_SAY_BYTES(Orig);
            LRC_SAY_BYTES(Moved);
            LRC_SAY_BYTES(PolyFirst);
            LRC_SAY_BYTES(PolyTris);
            LRC_SAY_BYTES(Tri);
            LRC_SAY_BYTES(TriTurn);
            LRC_SAY_BYTES(PolyFaceFirst);
            LRC_SAY_BYTES(PolyFaceCount);
            LRC_SAY_BYTES(PolyRelief);
            LRC_SAY_BYTES(PolyPlane);
            LRC_SAY_BYTES(FaceKind);
            LRC_SAY_BYTES(FaceCorner);
            LRC_SAY_BYTES(FaceTerm);
            LRC_SAY_BYTES(FacePlane);
            LRC_SAY_BYTES(PolyBox);
            LRC_SAY_BYTES(PolyGridFirst);
            LRC_SAY_BYTES(PolyGridCount);
            LRC_SAY_BYTES(PolyGridItems);
            LRC_SAY_BYTES(Left);
            LRC_SAY_BYTES(PolyEdgeKinds);
            LRC_SAY_BYTES(PolySideFirst);
            LRC_SAY_BYTES(PolySideCount);
            LRC_SAY_BYTES(LightSides);
            LRC_SAY_BYTES(LightParts);
            LRC_SAY_BYTES(Ring);
            LRC_SAY_BYTES(RingFirst);
            LRC_SAY_BYTES(RingCount);
            LRC_SAY_BYTES(RingOwn);
            LRC_SAY_BYTES(PolyChordFirst);
            LRC_SAY_BYTES(ChordFirst);
            LRC_SAY_BYTES(ChordCount);
            LRC_SAY_BYTES(ChordEnds);
            LRC_SAY_BYTES(ChordT);
            LRC_SAY_BYTES(ChordMove);
            LRC_SAY_BYTES(PlaceHeld);
            LRC_SAY_BYTES(PolyHeld);
            LRC_SAY_BYTES(BevelNormal);
#undef LRC_SAY_BYTES
            printf("; places %d, triangles %d\n", R.Orig.Num, R.Tri.Num / 3);
        }
        printf("bevel: %s; %d outward hard sides between faces it may cut, %d edges; left sharp: a free "
               "end %d, thin %d, touching %d (%d of them a mover's), no worn solid %d; %d polygons cut (%d triangles before, %d after), %d "
               "places put in, made %d times, ring sides missed %d, flat pieces %d, pieces turned %d (cut again where they moved %d); %.1f ms\n",
               bBevel ? "made" : "NO MEMORY", S.Sides, S.Edges, S.LeftFree, S.LeftThin, Bm.LeftTouching,
               Bm.LeftMovers, Bm.LeftWorn, Bm.Polygons, Bm.TrisBefore, Bm.TrisAfter, Bm.Points, Bm.Rounds, Bm.SidesMissed,
               Bm.PiecesFlat, Bm.PiecesTurned, Bm.PiecesCutMoved, BevelMs);
        // What the bevel made, to the bit: the places where they were and are, the triangles, each polygon's first and
        // how many (FNV-1a over their bytes).
        {
            unsigned long long Hash = 1469598103934665603ULL;
            auto               Mix  = [&](const void* Data, size_t Bytes) {
                const unsigned char* B = (const unsigned char*)Data;
                for (size_t i = 0; i < Bytes; i++)
                    Hash = (Hash ^ B[i]) * 1099511628211ULL;
            };
            Mix(R.Orig.Data, sizeof(FV3) * (size_t)R.Orig.Num);
            Mix(R.Moved.Data, sizeof(FV3) * (size_t)R.Moved.Num);
            Mix(R.Tri.Data, sizeof(int) * (size_t)R.Tri.Num);
            Mix(R.PolyFirst.Data, sizeof(int) * (size_t)R.PolyFirst.Num);
            Mix(R.PolyTris.Data, sizeof(int) * (size_t)R.PolyTris.Num);
            printf("bevel hash: %016llx\n", Hash);
        }
        printf("bevel parts, ms: shared %.1f, edges %.1f, lists %.1f, lines %.1f, moves %.1f, cuts %.1f, the cuts' points "
               "and the triangles %.1f; made %d times, %d of them as far as the cuts; faces cut again in their turn %d\n",
               Bm.PartMs[0], Bm.PartMs[1], Bm.PartMs[2], Bm.PartMs[3], Bm.PartMs[4], Bm.PartMs[5], Bm.PartMs[6], Bm.Rounds,
               Bm.CutRounds, Bm.CutAgain);
        // The rounding's own: every polygon it had moved keeps its triangles, and each of their places where it was -
        // to the last bit.
        long long OwnPolys = 0, OwnChanged = 0, OwnPlaces = 0, OwnMoved = 0;
        for (int p = 0; p < TrisWere.Num; p++)
        {
            if (TrisWere[p] <= 0)
                continue;
            OwnPolys++;
            const bool bSame = p < R.PolyTris.Num && R.PolyTris[p] == TrisWere[p] && R.PolyFirst[p] == FirstWere[p];
            if (!bSame)
            {
                OwnChanged++;
                continue;
            }
            for (int t = FirstWere[p] * 3; t < (FirstWere[p] + TrisWere[p]) * 3; t++)
            {
                const int w = TriWere[t];
                OwnPlaces++;
                if (R.Tri[t] != w || memcmp(&R.Moved[w], &MovedWere[w], sizeof(FV3)) != 0)
                    OwnMoved++;
            }
        }
        printf("bevel: the rounding's own polygons %lld, %lld of them changed; their triangles' corners %lld, %lld of them "
               "not as they were to the last bit\n", OwnPolys, OwnChanged, OwnPlaces, OwnMoved);
        // The check: every triangle's sides, one way round; and the sides of the polygons the bevel left alone.
        // Only the triangles the polygons draw: a moved polygon the bevel cut has a new run, its old one left behind.
        std::vector<int> Live;
        for (int p = 0; p < NumPolys && p < R.PolyTris.Num; p++)
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                Live.push_back(t * 3);
        // Where the bevel's triangles are: the still polygons it cut, and the moved ones it made again - how many, and
        // their triangles before and after.
        {
            long long StillN = 0, StillBefore = 0, StillAfter = 0, MovedN = 0, MovedBefore = 0, MovedAfter = 0;
            for (int p = 0; p < NumPolys && p < R.PolyTris.Num; p++)
            {
                const bool bWas = p < TrisWere.Num && TrisWere[p] > 0;
                if (!bWas && R.PolyTris[p] > 0)
                {
                    StillN++;
                    StillBefore += (p < R.RingCount.Num) ? R.RingCount[p] - 2 : 0;
                    StillAfter += R.PolyTris[p];
                }
                else if (bWas && R.PolyFirst[p] != FirstWere[p])
                {
                    MovedN++;
                    MovedBefore += TrisWere[p];
                    MovedAfter += R.PolyTris[p];
                }
            }
            printf("bevel triangles: still polygons cut %lld (%lld triangles before, %lld after), moved ones made again "
                   "%lld (%lld before, %lld after); the triangles' list %d long, the live ones %lld\n", StillN,
                   StillBefore, StillAfter, MovedN, MovedBefore, MovedAfter, R.Tri.Num / 3,
                   (long long)Live.size());
        }
        // A polygon the bevel cut: a still one that has triangles now, or a moved one whose triangles are a new run.
        auto Cut = [&](int p) {
            return p < R.PolyTris.Num && R.PolyTris[p] > 0 &&
                   (p >= TrisWere.Num || TrisWere[p] <= 0 || R.PolyFirst[p] != FirstWere[p]);
        };
        FKeySet TriSides, StillSides;
        for (const int t : Live)
            for (int k = 0; k < 3; k++)
                TriSides.Add(((long long)R.Tri[t + k] << 32) | (unsigned)R.Tri[t + (k + 1) % 3]);
        std::vector<unsigned char> Alone(NumPolys, 0);
        for (int p = 0; p < NumPolys; p++)
        {
            Alone[p] = (p >= R.PolyTris.Num || R.PolyTris[p] == 0) ? 1 : 0;
            if (!Alone[p] || p >= R.RingCount.Num)
                continue;
            const int Rf = R.RingFirst[p], Rc = R.RingCount[p];
            for (int i = 0; i < Rc; i++)
                StillSides.Add(((long long)R.Ring[Rf + i] << 32) | (unsigned)R.Ring[Rf + (i + 1) % Rc]);
        }
        // A side with no twin may still lie along another side, neither end of either moved - a polygon's left alone, or
        // a triangle's: a T-junction on a straight side, as the level has its own. Those sides by the cells their boxes
        // reach into.
        // Unmoved: the bevel did not move it, and the rounding not past a thousandth (its own small moves of the still
        // polygons' places, which the engine draws where they were).
        auto BevelMoved = [&](int w) {
            return (w < PlacesWere) ? RdxSphere::Length(R.Moved[w] - MovedWere[w]) > 1e-6
                                    : RdxSphere::Length(R.Moved[w] - R.Orig[w]) > 1e-6;
        };
        auto Unmoved = [&](int w) { return !BevelMoved(w) && RdxSphere::Length(R.Moved[w] - R.Orig[w]) <= 1e-3; };
        FCellMap AloneCells;
        std::vector<int>                                AloneSides;   // pairs of places
        auto AddStraight = [&](int U, int V) {
            const FV3& A = R.Orig[U];
            const FV3& B = R.Orig[V];
            const int  s = (int)AloneSides.size() / 2;
            AloneSides.push_back(U);
            AloneSides.push_back(V);
            const long long X0 = (long long)floor(RdxSphere::MinOf(A.X, B.X) / 16.0);
            const long long X1 = (long long)floor(RdxSphere::MaxOf(A.X, B.X) / 16.0);
            const long long Y0 = (long long)floor(RdxSphere::MinOf(A.Y, B.Y) / 16.0);
            const long long Y1 = (long long)floor(RdxSphere::MaxOf(A.Y, B.Y) / 16.0);
            const long long Z0 = (long long)floor(RdxSphere::MinOf(A.Z, B.Z) / 16.0);
            const long long Z1 = (long long)floor(RdxSphere::MaxOf(A.Z, B.Z) / 16.0);
            if ((X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1) > 4096)
                return;
            for (long long cz = Z0; cz <= Z1; cz++)
                for (long long cy = Y0; cy <= Y1; cy++)
                    for (long long cx = X0; cx <= X1; cx++)
                        AloneCells.Add(RdxLevelRound::Detail::CellKey(cx, cy, cz), s);
        };
        for (int p = 0; p < NumPolys && p < R.RingCount.Num; p++)
        {
            if (!Alone[p])
                continue;
            const int Rf = R.RingFirst[p], Rc = R.RingCount[p];
            for (int i = 0; i < Rc; i++)
                AddStraight(R.Ring[Rf + i], R.Ring[Rf + (i + 1) % Rc]);
        }
        for (const int t : Live)
            for (int k = 0; k < 3; k++)
                if (Unmoved(R.Tri[t + k]) && Unmoved(R.Tri[t + (k + 1) % 3]))
                    AddStraight(R.Tri[t + k], R.Tri[t + (k + 1) % 3]);
        auto Stretch = [](const FV3& P, const FV3& A, const FV3& B) {
            const FV3    D  = B - A;
            const double L2 = RdxSphere::Dot(D, D);
            const double T  = (L2 > 0.0) ? RdxSphere::MaxOf(0.0, RdxSphere::MinOf(1.0, RdxSphere::Dot(P - A, D) / L2)) : 0.0;
            return RdxSphere::Length(P - (A + D * T));
        };
        // Every wall's ring sides with their polygon, by cells: who was across a side before the bevel.
        FCellMap WallCells;
        std::vector<int>                                WallSides;   // triples: places, polygon
        for (int q = 0; q < NumPolys && q < R.RingCount.Num; q++)
        {
            const int Rf = R.RingFirst[q], Rc = R.RingCount[q];
            for (int i = 0; i < Rc; i++)
            {
                const int  U = R.Ring[Rf + i], V = R.Ring[Rf + (i + 1) % Rc];
                const FV3& Ua = R.Orig[U];
                const FV3& Vb = R.Orig[V];
                const int  s  = (int)WallSides.size() / 3;
                WallSides.push_back(U);
                WallSides.push_back(V);
                WallSides.push_back(q);
                const long long X0 = (long long)floor(RdxSphere::MinOf(Ua.X, Vb.X) / 16.0);
                const long long X1 = (long long)floor(RdxSphere::MaxOf(Ua.X, Vb.X) / 16.0);
                const long long Y0 = (long long)floor(RdxSphere::MinOf(Ua.Y, Vb.Y) / 16.0);
                const long long Y1 = (long long)floor(RdxSphere::MaxOf(Ua.Y, Vb.Y) / 16.0);
                const long long Z0 = (long long)floor(RdxSphere::MinOf(Ua.Z, Vb.Z) / 16.0);
                const long long Z1 = (long long)floor(RdxSphere::MaxOf(Ua.Z, Vb.Z) / 16.0);
                if ((X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1) > 4096)
                    continue;
                for (long long cz = Z0; cz <= Z1; cz++)
                    for (long long cy = Y0; cy <= Y1; cy++)
                        for (long long cx = X0; cx <= X1; cx++)
                            WallCells.Add(RdxLevelRound::Detail::CellKey(cx, cy, cz), s);
            }
        }
        // Every wall's ring sides as they came: a side of a cut polygon that is one, neither end moved, is as it was.
        FKeySet RingSidesWere;
        for (int q = 0; q < NumPolys && q < R.RingCount.Num; q++)
        {
            const int Rf = R.RingFirst[q], Rc = R.RingCount[q];
            for (int i = 0; i < Rc; i++)
                RingSidesWere.Add(((long long)R.Ring[Rf + i] << 32) | (unsigned)R.Ring[Rf + (i + 1) % Rc]);
        }
        // The sides of the polygons that are no walls (invisible, portals, mirrors, the sky's - no ring of theirs), from
        // the level's own points, by cells.
        FCellMap OtherCells;
        std::vector<FV3>                                OtherSides;   // pairs of points
        for (int q = 0; q < NumPolys; q++)
        {
            const FPolyIn& Q = Polys[q];
            if ((Q.Use & RdxSphere::Use_Wall) || Q.Count < 3)
                continue;
            for (int k = 0; k < Q.Count; k++)
            {
                const FV3& Ua = Pts[Q.First + k];
                const FV3& Vb = Pts[Q.First + (k + 1) % Q.Count];
                const int  s  = (int)OtherSides.size() / 2;
                OtherSides.push_back(Ua);
                OtherSides.push_back(Vb);
                const long long X0 = (long long)floor((RdxSphere::MinOf(Ua.X, Vb.X) - 0.05) / 16.0);
                const long long X1 = (long long)floor((RdxSphere::MaxOf(Ua.X, Vb.X) + 0.05) / 16.0);
                const long long Y0 = (long long)floor((RdxSphere::MinOf(Ua.Y, Vb.Y) - 0.05) / 16.0);
                const long long Y1 = (long long)floor((RdxSphere::MaxOf(Ua.Y, Vb.Y) + 0.05) / 16.0);
                const long long Z0 = (long long)floor((RdxSphere::MinOf(Ua.Z, Vb.Z) - 0.05) / 16.0);
                const long long Z1 = (long long)floor((RdxSphere::MaxOf(Ua.Z, Vb.Z) + 0.05) / 16.0);
                if ((X1 - X0 + 1) * (Y1 - Y0 + 1) * (Z1 - Z0 + 1) > 4096)
                    continue;
                for (long long cz = Z0; cz <= Z1; cz++)
                    for (long long cy = Y0; cy <= Y1; cy++)
                        for (long long cx = X0; cx <= X1; cx++)
                            OtherCells.Add(RdxLevelRound::Detail::CellKey(cx, cy, cz), s);
            }
        }
        // Every side as it is drawn - each live triangle's where the rounding and the bevel put its places, each ring side
        // of a polygon left alone where the engine draws it - by the 16-unit cells their boxes reach into: a side with no
        // twin whose two ends lie on one of them, the other way along, is a T-junction on a straight side, whatever moved
        // to get there - the rounding's own small moves of the places it shares with a still polygon among them.
        std::vector<int>                                   DrawnPlaces;   // pairs of places
        std::vector<unsigned char>                         DrawnOrig;     // 1: the engine's, where the places were
        std::vector<std::pair<long long, int>>             DrawnCells;
        // The 16-unit cells the side from A to B reaches into, Pad round it, each once: along it a piece of a cell at a
        // time - a long side across a level reaches into few of the cells its box does.
        std::vector<long long> Along;
        auto CellsAlong = [&Along](const FV3& A, const FV3& B, double Pad) {
            Along.clear();
            const int Pieces = (int)(RdxSphere::Length(B - A) / 16.0) + 1;
            for (int i = 0; i < Pieces; i++)
            {
                const FV3 P = A + (B - A) * ((double)i / Pieces), Q = A + (B - A) * ((double)(i + 1) / Pieces);
                const long long X0 = (long long)floor((RdxSphere::MinOf(P.X, Q.X) - Pad) / 16.0);
                const long long X1 = (long long)floor((RdxSphere::MaxOf(P.X, Q.X) + Pad) / 16.0);
                const long long Y0 = (long long)floor((RdxSphere::MinOf(P.Y, Q.Y) - Pad) / 16.0);
                const long long Y1 = (long long)floor((RdxSphere::MaxOf(P.Y, Q.Y) + Pad) / 16.0);
                const long long Z0 = (long long)floor((RdxSphere::MinOf(P.Z, Q.Z) - Pad) / 16.0);
                const long long Z1 = (long long)floor((RdxSphere::MaxOf(P.Z, Q.Z) + Pad) / 16.0);
                for (long long cz = Z0; cz <= Z1; cz++)
                    for (long long cy = Y0; cy <= Y1; cy++)
                        for (long long cx = X0; cx <= X1; cx++)
                            Along.push_back(RdxLevelRound::Detail::CellKey(cx, cy, cz));
            }
            std::sort(Along.begin(), Along.end());
            Along.erase(std::unique(Along.begin(), Along.end()), Along.end());
        };
        auto DrawnAt = [&](int s, int End) {
            const int w = DrawnPlaces[s * 2 + End];
            return DrawnOrig[s] ? R.Orig[w] : R.Moved[w];
        };
        auto AddDrawn = [&](int U, int V, unsigned char bOrig) {
            const int s = (int)DrawnOrig.size();
            DrawnPlaces.push_back(U);
            DrawnPlaces.push_back(V);
            DrawnOrig.push_back(bOrig);
            CellsAlong(DrawnAt(s, 0), DrawnAt(s, 1), 1e-3);
            for (const long long K : Along)
                DrawnCells.push_back({ K, s });
        };
        for (const int t : Live)
            for (int k = 0; k < 3; k++)
                AddDrawn(R.Tri[t + k], R.Tri[t + (k + 1) % 3], 0);
        for (int p = 0; p < NumPolys && p < R.RingCount.Num; p++)
            if (Alone[p] && (Polys[p].Use & RdxSphere::Use_Wall))
                for (int i = 0; i < R.RingCount[p]; i++)
                    AddDrawn(R.Ring[R.RingFirst[p] + i], R.Ring[R.RingFirst[p] + (i + 1) % R.RingCount[p]], 1);
        std::sort(DrawnCells.begin(), DrawnCells.end());
        // Whether the side from place A to B, drawn by the rounding's triangles, lies on another drawn side the other way.
        // Tol: how far off it a drawn side may lie, and the most of it left open.
        auto OnDrawn = [&](int A, int B, double Tol) {
            const FV3& Pa = R.Moved[A];
            const FV3& Pb = R.Moved[B];
            const long long K = RdxLevelRound::Detail::CellKey((long long)floor(Pa.X / 16.0), (long long)floor(Pa.Y / 16.0),
                                                               (long long)floor(Pa.Z / 16.0));
            for (auto It = std::lower_bound(DrawnCells.begin(), DrawnCells.end(), std::make_pair(K, -1));
                 It != DrawnCells.end() && It->first == K; ++It)
            {
                const FV3 U = DrawnAt(It->second, 0), V = DrawnAt(It->second, 1);
                if (RdxSphere::Dot(V - U, Pb - Pa) < 0.0 && Stretch(Pa, U, V) <= Tol && Stretch(Pb, U, V) <= Tol)
                    return true;
            }
            // Or the other way round: drawn sides the other way, each lying on it, cover it end to end - the side with
            // the T-junctions on it.
            const double L = RdxSphere::Length(Pb - Pa);
            if (!(L > 0.0))
                return false;
            const FV3 Way = (Pb - Pa) * (1.0 / L);
            std::vector<std::pair<double, double>> Spans;
            CellsAlong(Pa, Pb, Tol);
            for (const long long Kc : Along)
                for (auto It = std::lower_bound(DrawnCells.begin(), DrawnCells.end(), std::make_pair(Kc, -1));
                     It != DrawnCells.end() && It->first == Kc; ++It)
                {
                    const FV3 U = DrawnAt(It->second, 0), V = DrawnAt(It->second, 1);
                    if (RdxSphere::Dot(V - U, Way) < 0.0 && Stretch(U, Pa, Pb) <= Tol && Stretch(V, Pa, Pb) <= Tol)
                    {
                        const double Tu = RdxSphere::Dot(U - Pa, Way), Tv = RdxSphere::Dot(V - Pa, Way);
                        Spans.push_back({ RdxSphere::MinOf(Tu, Tv), RdxSphere::MaxOf(Tu, Tv) });
                    }
                }
            std::sort(Spans.begin(), Spans.end());
            double Reach = 0.0;
            for (const auto& Sp : Spans)
            {
                if (Sp.first > Reach + Tol)
                    return false;
                Reach = RdxSphere::MaxOf(Reach, Sp.second);
            }
            return Reach >= L - Tol;
        };
        TriSides.Done();
        StillSides.Done();
        AloneCells.Done();
        WallCells.Done();
        RingSidesWere.Done();
        OtherCells.Done();
        long long EdgeOther = 0;
        long long Open = 0, Sides = 0, TJunctions = 0, MovedAlone = 0, EdgeStill = 0, EdgeMoved = 0, AsWere = 0;
        long long Inner = 0;
        long long OpenNarrow = 0, InnerNarrow = 0;   // drawn within OnEdge of a side across: as the level's T-junctions
        int       SaidEdge = 0, SaidInner = 0;
        int       Said = 0;
        for (int p = 0; p < NumPolys && p < R.PolyTris.Num; p++)
        {
            if (!Cut(p))
                continue;
            for (int t = R.PolyFirst[p] * 3; t < (R.PolyFirst[p] + R.PolyTris[p]) * 3; t += 3)
                for (int k = 0; k < 3; k++)
                {
                    const int A = R.Tri[t + k], B = R.Tri[t + (k + 1) % 3];
                    Sides++;
                    const long long Back = ((long long)B << 32) | (unsigned)A;
                    if (TriSides.Has(Back) || StillSides.Has(Back))
                        continue;
                    bool bT = OnDrawn(A, B, 1e-3);
                    if (!bT && Unmoved(A) && Unmoved(B))
                    {
                        const FV3& Pa = R.Orig[A];
                        for (int s : AloneCells.At(RdxLevelRound::Detail::CellKey(
                                 (long long)floor(Pa.X / 16.0), (long long)floor(Pa.Y / 16.0), (long long)floor(Pa.Z / 16.0))))
                            {
                                const FV3& U = R.Orig[AloneSides[s * 2]];
                                const FV3& V = R.Orig[AloneSides[s * 2 + 1]];
                                // Another side, the other way along - as a neighbour's runs.
                                if (RdxSphere::Dot(V - U, R.Orig[B] - Pa) < 0.0 && Stretch(Pa, U, V) <= 1e-4 &&
                                    Stretch(R.Orig[B], U, V) <= 1e-4)
                                {
                                    bT = true;
                                    break;
                                }
                            }
                    }
                    if (bT)
                    {
                        TJunctions++;
                        continue;
                    }
                    // As it was: a side of the polygon's ring as it came, or a piece of one, neither end moved.
                    bool bWas = Unmoved(A) && Unmoved(B) && RingSidesWere.Has(((long long)A << 32) | (unsigned)B);
                    if (!bWas && Unmoved(A) && Unmoved(B) && p < R.RingCount.Num)
                    {
                        const int Rf = R.RingFirst[p], Rc = R.RingCount[p];
                        for (int i = 0; i < Rc && !bWas; i++)
                        {
                            const FV3& U = R.Orig[R.Ring[Rf + i]];
                            const FV3& V = R.Orig[R.Ring[Rf + (i + 1) % Rc]];
                            bWas = RdxSphere::Dot(V - U, R.Orig[B] - R.Orig[A]) > 0.0 &&
                                   Stretch(R.Orig[A], U, V) <= 1e-4 && Stretch(R.Orig[B], U, V) <= 1e-4;
                        }
                    }
                    if (bWas)
                    {
                        AsWere++;
                        continue;
                    }
                    // Open: a crack where a wall's ring had a side the other way along it, else the level's own edge -
                    // moved by the bevel or not.
                    int        Across = -1;
                    const FV3& Pa     = R.Orig[A];
                    for (int s : WallCells.At(RdxLevelRound::Detail::CellKey(
                             (long long)floor(Pa.X / 16.0), (long long)floor(Pa.Y / 16.0), (long long)floor(Pa.Z / 16.0))))
                        {
                            const FV3& U = R.Orig[WallSides[s * 3]];
                            const FV3& V = R.Orig[WallSides[s * 3 + 1]];
                            if (WallSides[s * 3 + 2] != p && RdxSphere::Dot(V - U, R.Orig[B] - Pa) < 0.0 &&
                                Stretch(Pa, U, V) <= 1e-3 && Stretch(R.Orig[B], U, V) <= 1e-3)
                            {
                                Across = WallSides[s * 3 + 2];
                                break;
                            }
                        }
                    if (Across < 0)
                    {
                        // The level's own edge lies along the polygon's ring - its points as near as the bevel takes
                        // two to be one; a side open inside it is a crack in it - a piece of the cut left out.
                        // On its plane: a place of a rounded wall's the ring took in may lie off it, the rounding
                        // having moved it onto the ring's side.
                        bool bOnRing = false;
                        if (p < R.RingCount.Num && R.RingCount[p] > 0)
                        {
                            const int    Rf = R.RingFirst[p], Rc = R.RingCount[p];
                            const FV3    N  = RdxSphere::V3(Polys[p].Nx, Polys[p].Ny, Polys[p].Nz);
                            const double D  = RdxSphere::Dot(N, R.Orig[R.Ring[Rf]]);
                            auto         On = [&](const FV3& X) { return X - N * (RdxSphere::Dot(N, X) - D); };
                            const FV3    Qa = On(Pa), Qb = On(R.Orig[B]);
                            for (int i = 0; i < Rc && !bOnRing; i++)
                            {
                                const FV3 U = On(R.Orig[R.Ring[Rf + i]]);
                                const FV3 V = On(R.Orig[R.Ring[Rf + (i + 1) % Rc]]);
                                bOnRing = Stretch(Qa, U, V) <= RdxLevelRound::WeldDist &&
                                          Stretch(Qb, U, V) <= RdxLevelRound::WeldDist;
                            }
                        }
                        if (!bOnRing)
                        {
                            Inner++;
                            InnerNarrow += OnDrawn(A, B, RdxLevelRound::OnEdge) ? 1 : 0;
                            if (SaidInner++ < 5)
                                printf("  bevel: a crack inside polygon %d from %.3f %.3f %.3f to %.3f %.3f %.3f (places %d, "
                                       "%d)\n", p, Pa.X, Pa.Y, Pa.Z, R.Orig[B].X, R.Orig[B].Y, R.Orig[B].Z, A, B);
                            continue;
                        }
                        if (Unmoved(A) && Unmoved(B))
                        {
                            EdgeStill++;
                            continue;
                        }
                        EdgeMoved++;
                        // Against a polygon that is no wall (invisible, a portal, a mirror, the sky's): it stays.
                        bool bOther = false;
                        for (int s : OtherCells.At(RdxLevelRound::Detail::CellKey(
                                 (long long)floor(Pa.X / 16.0), (long long)floor(Pa.Y / 16.0), (long long)floor(Pa.Z / 16.0))))
                                if (Stretch(Pa, OtherSides[s * 2], OtherSides[s * 2 + 1]) <= 0.05 &&
                                    Stretch(R.Orig[B], OtherSides[s * 2], OtherSides[s * 2 + 1]) <= 0.05)
                                {
                                    bOther = true;
                                    break;
                                }
                        EdgeOther += bOther ? 1 : 0;
                        if (bOther && SaidEdge++ < 3)
                            printf("  bevel: the level's edge moved against a polygon that is no wall, at polygon %d, "
                                   "from %.3f %.3f %.3f to %.3f %.3f %.3f\n", p, Pa.X, Pa.Y, Pa.Z, R.Orig[B].X,
                                   R.Orig[B].Y, R.Orig[B].Z);
                        continue;
                    }
                    Open++;
                    const bool bNarrow = OnDrawn(A, B, RdxLevelRound::OnEdge);
                    OpenNarrow += bNarrow ? 1 : 0;
                    if (!bNarrow && Said++ < 5)
                        printf("  bevel: a crack at polygon %d from %.3f %.3f %.3f to %.3f %.3f %.3f (places %d, %d; "
                               "moved %d %d), across polygon %d (use %d, tris %d, was %d)\n", p, Pa.X, Pa.Y, Pa.Z,
                               R.Orig[B].X, R.Orig[B].Y, R.Orig[B].Z, A, B, Unmoved(A) ? 0 : 1, Unmoved(B) ? 0 : 1,
                               Across, Polys[Across].Use, (Across < R.PolyTris.Num) ? R.PolyTris[Across] : 0,
                               (Across < TrisWere.Num) ? TrisWere[Across] : 0);
                }
        }
        for (int p = 0; p < NumPolys && p < R.RingCount.Num; p++)
        {
            if (!Alone[p])
                continue;
            const int Rf = R.RingFirst[p], Rc = R.RingCount[p];
            for (int i = 0; i < Rc; i++)
            {
                const int w = R.Ring[Rf + i];
                if (w < PlacesWere && RdxSphere::Length(R.Moved[w] - MovedWere[w]) > 0.0)
                    MovedAlone++;
            }
        }
        printf("bevel check: %lld sides of the cut polygons' triangles; cracks %lld (%lld within OnEdge), inside the "
               "polygons %lld (%lld within OnEdge); T-junctions on a straight side %lld; sides as they were %lld; the "
               "level's own edge %lld still, %lld moved (%lld of them against a polygon that is no wall); places the bevel "
               "moved under polygons left alone %lld\n",
               Sides, Open, OpenNarrow, Inner, InnerNarrow, TJunctions, AsWere, EdgeStill, EdgeMoved, EdgeOther,
               MovedAlone);
        // Where the cut polygons' corners lie: each where it was on its polygon's plane and inside its box, and moved by
        // no more than the ball's radius and a little - a triangle corner past either is a piece drawn where the level
        // has nothing (the rays' scene takes the same triangles).
        {
            long long Corners = 0, OffPlane = 0, OffBox = 0, FarMoved = 0;
            double    WorstPlane = 0.0, WorstBox = 0.0, WorstMove = 0.0;
            int       WorstPoly = -1, WorstPlace = -1;
            long long NewOnMoved = 0, PastCap = 0;
            // LRC_DETAIL_BOX=x0,y0,z0,x1,y1,z1: each corner there that the bevel moved by more than its radius, said.
            double FarBox[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            const char* const FarBoxText = getenv("LRC_DETAIL_BOX");
            const bool bFarBox = FarBoxText && sscanf(FarBoxText, "%lf,%lf,%lf,%lf,%lf,%lf", &FarBox[0], &FarBox[1],
                                                      &FarBox[2], &FarBox[3], &FarBox[4], &FarBox[5]) == 6;
            for (int p = 0; p < NumPolys && p < R.PolyTris.Num; p++)
            {
                if (!Cut(p))
                    continue;
                const bool     bWasMoved = p < TrisWere.Num && TrisWere[p] > 0;
                const FPolyIn& P = Polys[p];
                FV3 Lo = Pts[P.First], Hi = Pts[P.First];
                for (int k = 1; k < P.Count; k++)
                {
                    const FV3& Q = Pts[P.First + k];
                    Lo = RdxSphere::V3(Q.X < Lo.X ? Q.X : Lo.X, Q.Y < Lo.Y ? Q.Y : Lo.Y, Q.Z < Lo.Z ? Q.Z : Lo.Z);
                    Hi = RdxSphere::V3(Q.X > Hi.X ? Q.X : Hi.X, Q.Y > Hi.Y ? Q.Y : Hi.Y, Q.Z > Hi.Z ? Q.Z : Hi.Z);
                }
                for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                    for (int k = 0; k < 3; k++)
                    {
                        const int  v     = R.Tri[t * 3 + k];
                        const FV3& O     = R.Orig[v];
                        const double Pl  = fabs(O.X * P.Nx + O.Y * P.Ny + O.Z * P.Nz - P.W);
                        const double Out[6] = { Lo.X - O.X, O.X - Hi.X, Lo.Y - O.Y, O.Y - Hi.Y, Lo.Z - O.Z, O.Z - Hi.Z };
                        double       Bx     = 0.0;
                        for (int a = 0; a < 6; a++)
                            Bx = (Out[a] > Bx) ? Out[a] : Bx;
                        // The bevel's own move: a place the rounding had, from where the rounding put it; one the
                        // bevel put on a still polygon, from where it lay. One it put on a moved polygon's triangle came
                        // where the rounding drew that triangle, which is not kept: held to the rounding's cap and the
                        // ball (PastCap) instead.
                        double Mv = (v < PlacesWere) ? RdxSphere::Length(R.Moved[v] - MovedWere[v])
                                                     : RdxSphere::Length(R.Moved[v] - O);
                        if (v >= PlacesWere && bWasMoved)
                        {
                            NewOnMoved++;
                            PastCap += (Mv > Cap + 1.5) ? 1 : 0;
                            Mv = 0.0;
                        }
                        if (bFarBox && Mv > 1.0 && O.X >= FarBox[0] && O.X <= FarBox[3] && O.Y >= FarBox[1] &&
                            O.Y <= FarBox[4] && O.Z >= FarBox[2] && O.Z <= FarBox[5])
                            printf("  bevel moved far: place %d (%s) of polygon %d at %.2f %.2f %.2f, the rounding's move "
                                   "%.2f, the bevel's %.2f to %.2f %.2f %.2f\n", v,
                                   (v < PlacesWere) ? "the rounding's" : "put in by the bevel", p, O.X, O.Y, O.Z,
                                   (v < PlacesWere) ? RdxSphere::Length(MovedWere[v] - O) : -1.0, Mv, R.Moved[v].X,
                                   R.Moved[v].Y, R.Moved[v].Z);
                        Corners++;
                        OffPlane += (Pl > 0.05) ? 1 : 0;
                        OffBox += (Bx > 0.05) ? 1 : 0;
                        FarMoved += (Mv > 1.5) ? 1 : 0;
                        if (Pl > WorstPlane || Bx > WorstBox || Mv > WorstMove)
                            WorstPoly = p;
                        if (Mv > WorstMove)
                            WorstPlace = v;
                        WorstPlane = (Pl > WorstPlane) ? Pl : WorstPlane;
                        WorstBox   = (Bx > WorstBox) ? Bx : WorstBox;
                        WorstMove  = (Mv > WorstMove) ? Mv : WorstMove;
                    }
            }
            // The places the faces kept from most of their round's move (LevelBevel.h's LevelBevelKeptCounts).
            printf("bevel kept: %lld moves of places the faces kept from most of their round, %lld of them whole\n",
                   RdxLevelRound::BevelDetail::LevelBevelKeptCounts()[0],
                   RdxLevelRound::BevelDetail::LevelBevelKeptCounts()[1]);
            printf("bevel places: %lld corners of the cut polygons' triangles; off their plane by more than 0.05 %lld (the "
                   "most %.3f), outside their box %lld (the most %.3f), moved more than 1.5 %lld (the most %.3f); the "
                   "worst polygon %d; of the places it put on moved polygons (%lld) %lld past the cap and 1.5\n", Corners,
                   OffPlane, WorstPlane, OffBox, WorstBox, FarMoved, WorstMove, WorstPoly, NewOnMoved, PastCap);
            if (WorstPlace >= 0)
                printf("bevel places: the place moved furthest, %d (%s): was %.3f %.3f %.3f, is %.3f %.3f %.3f\n", WorstPlace,
                       (WorstPlace < PlacesWere) ? "the rounding's" : "put in by the bevel", R.Orig[WorstPlace].X,
                       R.Orig[WorstPlace].Y, R.Orig[WorstPlace].Z, R.Moved[WorstPlace].X, R.Moved[WorstPlace].Y,
                       R.Moved[WorstPlace].Z);
        }
        // LRC_POLY=<p>: polygon p's ring as it came, and its triangles after - places and where they are.
        // LRC_POLYM=<p>,<p>...: each polygon's triangles where they are drawn - each corner where the rounding put it
        // and how far the bevel moved it from there (a new place of the bevel's: -1) - and the polygon's plane.
        if (const char* Look = getenv("LRC_POLYM"))
            for (const char* C = Look; *C;)
            {
                const int p = atoi(C);
                while (*C && *C != ',')
                    C++;
                if (*C == ',')
                    C++;
                if (p < 0 || p >= NumPolys || p >= R.PolyTris.Num)
                    continue;
                printf("polygon %d: normal %.4f %.4f %.4f w %.3f, use %d, %d triangles\n", p, Polys[p].Nx, Polys[p].Ny,
                       Polys[p].Nz, Polys[p].W, Polys[p].Use, R.PolyTris[p]);
                for (int t = R.PolyFirst[p] * 3; t < (R.PolyFirst[p] + R.PolyTris[p]) * 3; t += 3)
                {
                    printf("  tri");
                    for (int k = 0; k < 3; k++)
                    {
                        const int  w = R.Tri[t + k];
                        const FV3& Q = R.Moved[w];
                        printf(" [%d %.3f %.3f %.3f b%.3f]", w, Q.X, Q.Y, Q.Z,
                               (w < PlacesWere) ? RdxSphere::Length(R.Moved[w] - MovedWere[w]) : -1.0);
                    }
                    printf("\n");
                }
            }
        if (const char* Look = getenv("LRC_POLY"))
        {
            const int p = atoi(Look);
            if (p >= 0 && p < NumPolys && p < R.RingCount.Num)
            {
                const int Rf = R.RingFirst[p], Rc = R.RingCount[p];
                printf("polygon %d: normal %.4f %.4f %.4f, ring of %d:", p, Polys[p].Nx, Polys[p].Ny, Polys[p].Nz, Rc);
                for (int i = 0; i < Rc; i++)
                {
                    const FV3& Q = R.Orig[R.Ring[Rf + i]];
                    printf(" [%d %.3f %.3f %.3f]", R.Ring[Rf + i], Q.X, Q.Y, Q.Z);
                }
                printf("\n");
                // The ring's area and its triangles' together, where they lay on the plane, signed along the normal: a
                // piece of it no triangle covers makes the second the smaller.
                {
                    const FV3 N = RdxSphere::V3(Polys[p].Nx, Polys[p].Ny, Polys[p].Nz);
                    double    Ring = 0.0, Tris = 0.0;
                    for (int i = 1; i + 1 < Rc; i++)
                        Ring += 0.5 * RdxSphere::Dot(RdxSphere::Cross(R.Orig[R.Ring[Rf + i]] - R.Orig[R.Ring[Rf]],
                                                                      R.Orig[R.Ring[Rf + i + 1]] - R.Orig[R.Ring[Rf]]), N);
                    if (p < R.PolyTris.Num)
                        for (int t = R.PolyFirst[p] * 3; t < (R.PolyFirst[p] + R.PolyTris[p]) * 3; t += 3)
                            Tris += 0.5 * RdxSphere::Dot(RdxSphere::Cross(R.Orig[R.Tri[t + 1]] - R.Orig[R.Tri[t]],
                                                                          R.Orig[R.Tri[t + 2]] - R.Orig[R.Tri[t]]), N);
                    printf("polygon %d: the ring's area %.4f, its triangles' %.4f\n", p, Ring, Tris);
                }
                if (p < R.PolyTris.Num)
                    for (int t = R.PolyFirst[p] * 3; t < (R.PolyFirst[p] + R.PolyTris[p]) * 3; t += 3)
                    {
                        printf("  tri");
                        for (int k = 0; k < 3; k++)
                        {
                            const FV3& Q = R.Orig[R.Tri[t + k]];
                            printf(" [%d %.3f %.3f %.3f]", R.Tri[t + k], Q.X, Q.Y, Q.Z);
                        }
                        printf("\n");
                    }
            }
        }
        // LRC_PLACES=<w>,<w>...: what each of those places is - where, moved how far, and every polygon whose ring has it
        // or a side through it, with the polygon's use, its triangles before and after.
        if (const char* Look = getenv("LRC_PLACES"))
            for (const char* C = Look; *C;)
            {
                const int w = atoi(C);
                while (*C && *C != ',')
                    C++;
                if (*C == ',')
                    C++;
                if (w < 0 || w >= R.Orig.Num)
                    continue;
                const FV3& P = R.Orig[w];
                printf("place %d at %.4f %.4f %.4f, moved %.4f (before the bevel %.4f)\n", w, P.X, P.Y, P.Z,
                       RdxSphere::Length(R.Moved[w] - P),
                       (w < (int)MovedWere.size()) ? RdxSphere::Length(MovedWere[w] - P) : -1.0);
                for (int q = 0; q < NumPolys && q < R.RingCount.Num; q++)
                {
                    const int Rf = R.RingFirst[q], Rc = R.RingCount[q];
                    for (int i = 0; i < Rc; i++)
                    {
                        const int  U = R.Ring[Rf + i], V = R.Ring[Rf + (i + 1) % Rc];
                        const bool bOn = U == w;
                        const bool bThrough = !bOn && V != w && Stretch(P, R.Orig[U], R.Orig[V]) <= 0.05;
                        if (bOn || bThrough)
                            printf("  polygon %d (use %d, tris %d, was %d): %s %d-%d\n", q, Polys[q].Use,
                                   (q < R.PolyTris.Num) ? R.PolyTris[q] : 0, (q < TrisWere.Num) ? TrisWere[q] : 0,
                                   bOn ? "its ring's place, the side" : "a side passes by:", U, V);
                    }
                }
            }
    }
    // With "tris <file> <polygons...>" after the angle: the named polygons' triangles written to the file, a line each -
    // the polygon, then each corner where it was and where it is (x y z x y z) - for a picture of them from above.
    if (Argc > 5 && strcmp(Argv[3], "tris") == 0)
    {
        FILE* const O = fopen(Argv[4], "w");
        if (!O)
        {
            printf("cannot write %s\n", Argv[4]);
            return 1;
        }
        for (int a = 5; a < Argc; a++)
        {
            const int p = atoi(Argv[a]);
            if (p < 0 || p >= NumPolys)
                continue;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                fprintf(O, "%d", p);
                for (int j = 0; j < 3; j++)
                {
                    const FV3& Q = R.Orig[R.Tri[t * 3 + j]];
                    const FV3& M = R.Moved[R.Tri[t * 3 + j]];
                    fprintf(O, " %.4f %.4f %.4f %.4f %.4f %.4f", Q.X, Q.Y, Q.Z, M.X, M.Y, M.Z);
                }
                fprintf(O, "\n");
            }
        }
        fclose(O);
        printf("tris: written to %s\n", Argv[4]);
    }
    // With "polymoves" and a file after the angle: each drawn polygon's most moved place, its middle and its way, a line
    // a polygon - two runs' files told apart show where a rule changes the rounding.
    if (Argc > 4 && strcmp(Argv[3], "polymoves") == 0)
    {
        FILE* const O = fopen(Argv[4], "w");
        if (!O)
        {
            printf("cannot write %s\n", Argv[4]);
            return 1;
        }
        for (int p = 0; p < NumPolys && p < R.PolyTris.Num; p++)
        {
            if (R.PolyTris[p] <= 0)
                continue;
            double Most = 0.0;
            FV3    Mid  = RdxSphere::V3(0.0, 0.0, 0.0);
            int    N    = 0;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                for (int j = 0; j < 3; j++)
                {
                    const int k = R.Tri[t * 3 + j];
                    Most        = fmax(Most, RdxSphere::Length(R.Moved[k] - R.Orig[k]));
                    Mid         = Mid + R.Orig[k];
                    N++;
                }
            Mid = Mid * (1.0 / N);
            fprintf(O, "%d %.4f %.1f %.1f %.1f %.2f %.2f %.2f\n", p, Most, Mid.X, Mid.Y, Mid.Z, Polys[p].Nx, Polys[p].Ny,
                    Polys[p].Nz);
        }
        fclose(O);
        printf("polymoves: written to %s\n", Argv[4]);
    }
    // With "chords" after the angle: the relief's outlines (ChordsOf) - each record's points among its wall's triangles'
    // places as moved, the card's copy (PackedChordAt) at each point and halfway between two, and of the walls with
    // none, why: their triangles' sides no other of theirs has, the places two of those start at, the corners not on
    // them.
    if (Argc > 3 && strcmp(Argv[3], "chords") == 0)
    {
        RdxSphere::TList<unsigned int> Words;
        const bool bPacked = RdxLevelRound::PackRound(R, Words);
        int    Records = 0, Points = 0, NotOn = 0, Shown = 0;
        double Off     = 0.0;
        for (int p = 0; p < NumPolys; p++)
        {
            const FPolyIn& P = Polys[p];
            if (!(P.Use & RdxSphere::Use_Relief) || R.PolyTris[p] <= 0)
                continue;
            if (R.PolyChordFirst[p] >= 0)
            {
                for (int k = 0; k < P.Count; k++)
                {
                    const int            e = R.PolyChordFirst[p] + k, F0 = R.ChordFirst[e], N = R.ChordCount[e];
                    const RdxSphere::FV3 A = R.ChordEnds[2 * e], B = R.ChordEnds[2 * e + 1];
                    Records++;
                    for (int i = 0; i < N; i++)
                    {
                        Points++;
                        const RdxSphere::FV3 At = A + (B - A) * R.ChordT[F0 + i], Moved = At + R.ChordMove[F0 + i];
                        bool bOn = false;
                        for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p] && !bOn; t++)
                            for (int j = 0; j < 3 && !bOn; j++)
                                bOn = RdxSphere::Length(R.Moved[R.Tri[t * 3 + j]] - Moved) < 0.02;
                        NotOn += bOn ? 0 : 1;
                        RdxSphere::FV3 G;
                        if (bPacked && RdxLevelRound::PackedChordAt(Words, (unsigned int)e, At, G))
                            Off = fmax(Off, RdxSphere::Length(G - R.ChordMove[F0 + i]));
                        else
                            Off = 1e9;
                        if (i + 1 < N)
                        {
                            const RdxSphere::FV3 Next = A + (B - A) * R.ChordT[F0 + i + 1];
                            const RdxSphere::FV3 Half = (R.ChordMove[F0 + i] + R.ChordMove[F0 + i + 1]) * 0.5;
                            if (bPacked && RdxLevelRound::PackedChordAt(Words, (unsigned int)e, (At + Next) * 0.5, G))
                                Off = fmax(Off, RdxSphere::Length(G - Half));
                            else
                                Off = 1e9;
                        }
                    }
                }
                continue;
            }
            // A wall with none: its sides no other of its triangles has, and where they start.
            RdxSphere::TList<int> From;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                for (int j = 0; j < 3; j++)
                {
                    const int a = R.Tri[t * 3 + j], b = R.Tri[t * 3 + (j + 1) % 3];
                    bool      bBack = false;
                    for (int u = R.PolyFirst[p]; u < R.PolyFirst[p] + R.PolyTris[p] && !bBack; u++)
                        for (int m = 0; m < 3 && !bBack; m++)
                            bBack = R.Tri[u * 3 + m] == b && R.Tri[u * 3 + (m + 1) % 3] == a;
                    if (!bBack)
                        From.Add(a);
                }
            const int Ring = From.Num;
            int       Twice = 0, Lost = 0;
            for (int i = 0; i < From.Num; i++)
                for (int j = 0; j < From.Num; j++)
                    if (j != i && From[j] == From[i])
                    {
                        Twice++;
                        break;
                    }
            for (int k = 0; k < P.Count; k++)
            {
                bool bFound = false;
                for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p] && !bFound; t++)
                    for (int j = 0; j < 3 && !bFound; j++)
                        bFound = RdxSphere::Length(R.Orig[R.Tri[t * 3 + j]] - Pts[P.First + k]) <= 0.02;
                Lost += bFound ? 0 : 1;
            }
            if (Shown++ < 12)
                printf("  chords: wall %d, %d corners, %d triangles, %d sides round it, %d of them from a place two "
                       "start at, %d corners on none of its places; first corner %.1f %.1f %.1f\n",
                       p, P.Count, R.PolyTris[p], Ring, Twice, Lost, Pts[P.First].X, Pts[P.First].Y, Pts[P.First].Z);
            // The first two in full: the corners, and the sides round it where they start.
            if (Shown <= 2)
            {
                for (int k = 0; k < P.Count; k++)
                    printf("    corner %d: %.3f %.3f %.3f\n", k, Pts[P.First + k].X, Pts[P.First + k].Y,
                           Pts[P.First + k].Z);
                for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                    for (int j = 0; j < 3; j++)
                    {
                        const int a = R.Tri[t * 3 + j], b = R.Tri[t * 3 + (j + 1) % 3];
                        bool      bBack = false;
                        for (int u = R.PolyFirst[p]; u < R.PolyFirst[p] + R.PolyTris[p] && !bBack; u++)
                            for (int m = 0; m < 3 && !bBack; m++)
                                bBack = R.Tri[u * 3 + m] == b && R.Tri[u * 3 + (m + 1) % 3] == a;
                        if (!bBack)
                            printf("    side %d -> %d: %.3f %.3f %.3f -> %.3f %.3f %.3f\n", a, b, R.Orig[a].X,
                                   R.Orig[a].Y, R.Orig[a].Z, R.Orig[b].X, R.Orig[b].Y, R.Orig[b].Z);
                    }
            }
        }
        printf("chords: %d walls with records (%d records, %d points; %d points on none of their triangles' places, the "
               "card's copy off by %.6f at most), %d walls with none\n",
               R.ChordPolys, Records, Points, NotOn, Off, R.ChordMissed);
        // Of the walls with records, those with a triangle turned over (against its place before) that has a corner on
        // the outline: there what the triangles cover reaches past the ring the records follow.
        int FoldedWalls = 0, FoldedTris = 0, Listed = 0;
        for (int p = 0; p < NumPolys; p++)
        {
            if (R.PolyChordFirst[p] < 0)
                continue;
            int Folds = 0;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                const int            A = R.Tri[t * 3], B = R.Tri[t * 3 + 1], C = R.Tri[t * 3 + 2];
                const RdxSphere::FV3 Now = RdxSphere::Cross(R.Moved[B] - R.Moved[A], R.Moved[C] - R.Moved[A]);
                const RdxSphere::FV3 Was = RdxSphere::Cross(R.Orig[B] - R.Orig[A], R.Orig[C] - R.Orig[A]);
                if (RdxSphere::Dot(Now, Was) >= 0.0)
                    continue;
                bool bOnRing = false;
                for (int k = 0; k < Polys[p].Count && !bOnRing; k++)
                {
                    const int e = R.PolyChordFirst[p] + k;
                    for (int i = R.ChordFirst[e]; i < R.ChordFirst[e] + R.ChordCount[e] && !bOnRing; i++)
                    {
                        const RdxSphere::FV3 At = R.ChordEnds[2 * e] + (R.ChordEnds[2 * e + 1] - R.ChordEnds[2 * e]) * R.ChordT[i];
                        for (int j = 0; j < 3 && !bOnRing; j++)
                            bOnRing = RdxSphere::Length(R.Orig[R.Tri[t * 3 + j]] - At) < 0.06;
                    }
                }
                Folds += bOnRing ? 1 : 0;
            }
            if (Folds > 0)
            {
                FoldedWalls++;
                FoldedTris += Folds;
                if (Listed++ < 8)
                    printf("  chords: wall %d has %d triangles turned over at its outline; first corner %.1f %.1f %.1f\n",
                           p, Folds, Pts[Polys[p].First].X, Pts[Polys[p].First].Y, Pts[Polys[p].First].Z);
            }
        }
        printf("chords: %d walls with records have triangles turned over at their outline (%d triangles)\n", FoldedWalls,
               FoldedTris);
        // And the polygons named after "chords", each edge's record in full, and their triangles where they were and
        // where they are.
        for (int a = 4; a < Argc; a++)
        {
            const int p = atoi(Argv[a]);
            if (p < 0 || p >= NumPolys)
                continue;
            const FPolyIn& P = Polys[p];
            printf("  polygon %d: use %d, %d corners, %d triangles, first record %d\n", p, P.Use, P.Count, R.PolyTris[p],
                   R.PolyChordFirst[p]);
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                printf("    triangle %d:", t - R.PolyFirst[p]);
                for (int j = 0; j < 3; j++)
                {
                    const FV3& O = R.Orig[R.Tri[t * 3 + j]];
                    const FV3& M = R.Moved[R.Tri[t * 3 + j]];
                    printf(" [%d %.2f %.2f %.2f -> %.2f %.2f %.2f]", R.Tri[t * 3 + j], O.X, O.Y, O.Z, M.X, M.Y, M.Z);
                }
                printf("\n");
            }
            for (int k = 0; k < P.Count; k++)
            {
                const FV3& Q = Pts[P.First + k];
                printf("    corner %d: %.2f %.2f %.2f", k, Q.X, Q.Y, Q.Z);
                if (R.PolyChordFirst[p] >= 0)
                {
                    const int e = R.PolyChordFirst[p] + k;
                    printf(" - edge to the next: %d points;", R.ChordCount[e]);
                    for (int i = R.ChordFirst[e]; i < R.ChordFirst[e] + R.ChordCount[e]; i++)
                        printf(" %.3f(%.2f %.2f %.2f)", R.ChordT[i], R.ChordMove[i].X, R.ChordMove[i].Y, R.ChordMove[i].Z);
                }
                printf("\n");
            }
        }
    }
    // With "quadfloat" after the angle: every quad face sampled on a 33 x 33 grid of its U and V, the card's float solve
    // held against the double one - where it finds no place, or another one.
    if (Argc > 3 && strcmp(Argv[3], "quadfloat") == 0)
    {
        int Quads = 0, Fails = 0, Off = 0, Said = 0;
        double Worst = 0.0;
        for (int f = 0; f < R.FaceKind.Num; f++)
        {
            if (R.FaceKind[f] == 0)
                continue;
            Quads++;
            const FV3* Q = &R.FaceCorner[f * 4];
            for (int i = 0; i <= 32; i++)
                for (int j = 0; j <= 32; j++)
                {
                    const double U = i / 32.0, V = j / 32.0;
                    const FV3 P = Q[0] * ((1 - U) * (1 - V)) + Q[1] * (U * (1 - V)) + Q[2] * (U * V) + Q[3] * ((1 - U) * V);
                    float Uf = -1.0f, Vf = -1.0f;
                    const bool bSolved = QuadUvFloat(Q, R.FaceKind[f] - 1, P, Uf, Vf);
                    const double E = bSolved ? fmax(fabs(Uf - U), fabs(Vf - V)) : 1e9;
                    if (!bSolved || Uf < -1e-3f || Uf > 1.001f || Vf < -1e-3f || Vf > 1.001f)
                    {
                        Fails++;
                        if (Said < 10)
                        {
                            Said++;
                            printf("  quadfloat: face %d at U %.3f V %.3f: %s (U %.4f V %.4f); corners %.1f %.1f %.1f / %.1f %.1f %.1f / "
                                   "%.1f %.1f %.1f / %.1f %.1f %.1f\n", f, U, V, bSolved ? "outside" : "no place", Uf, Vf,
                                   Q[0].X, Q[0].Y, Q[0].Z, Q[1].X, Q[1].Y, Q[1].Z, Q[2].X, Q[2].Y, Q[2].Z, Q[3].X, Q[3].Y, Q[3].Z);
                        }
                    }
                    else if (E > 1e-3)
                        Off++;
                    if (bSolved)
                        Worst = fmax(Worst, E);
                }
        }
        printf("quadfloat: %d quad faces, %d samples found no place or one outside, %d off by more than a thousandth, the worst "
               "%.5f\n", Quads, Fails, Off, Worst);
    }
    // With "obj X0 Y0 Z0 X1 Y1 Z1 <out>" after the angle: the rounded polygons whose box meets that box written out to be
    // looked at - their triangles as the engine would lay them (group "flat") and as they are moved (group "round"), and
    // the polygons that stay as they are in both.
    if (Argc > 10 && strcmp(Argv[3], "obj") == 0)
    {
        const double B[6] = { atof(Argv[4]), atof(Argv[5]), atof(Argv[6]), atof(Argv[7]), atof(Argv[8]), atof(Argv[9]) };
        FILE*        O    = fopen(Argv[10], "w");
        if (!O)
        {
            printf("cannot write %s\n", Argv[10]);
            return 2;
        }
        int Shown = 0, Next = 1;
        for (int g = 0; g < 2; g++)
        {
            fprintf(O, "g %s\n", g ? "round" : "flat");
            for (int p = 0; p < NumPolys; p++)
            {
                const FPolyIn& P = Polys[p];
                if (P.Use == 0 || P.Count < 3)
                    continue;
                double X0 = 1e30, X1 = -1e30, Y0 = 1e30, Y1 = -1e30, Z0 = 1e30, Z1 = -1e30;
                for (int k = 0; k < P.Count; k++)
                {
                    const FV3& Q = Pts[P.First + k];
                    X0 = fmin(X0, Q.X); X1 = fmax(X1, Q.X);
                    Y0 = fmin(Y0, Q.Y); Y1 = fmax(Y1, Q.Y);
                    Z0 = fmin(Z0, Q.Z); Z1 = fmax(Z1, Q.Z);
                }
                if (X1 < B[0] || X0 > B[3] || Y1 < B[1] || Y0 > B[4] || Z1 < B[2] || Z0 > B[5])
                    continue;
                Shown += g ? 0 : 1;
                fprintf(O, "o poly%d\n", p);
                const int Tris = (p < R.PolyTris.Num) ? R.PolyTris[p] : 0;
                if (Tris > 0)
                    for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + Tris; t++)
                    {
                        for (int k = 0; k < 3; k++)
                        {
                            const FV3& Q = g ? R.Moved[R.Tri[t * 3 + k]] : R.Orig[R.Tri[t * 3 + k]];
                            fprintf(O, "v %.4f %.4f %.4f\n", Q.X, Q.Y, Q.Z);
                        }
                        fprintf(O, "f %d %d %d\n", Next, Next + 1, Next + 2);
                        Next += 3;
                    }
                else
                {
                    for (int k = 0; k < P.Count; k++)
                        fprintf(O, "v %.4f %.4f %.4f\n", Pts[P.First + k].X, Pts[P.First + k].Y, Pts[P.First + k].Z);
                    for (int k = 1; k + 1 < P.Count; k++)
                        fprintf(O, "f %d %d %d\n", Next, Next + k, Next + k + 1);
                    Next += P.Count;
                }
            }
        }
        fclose(O);
        printf("obj: %d polygons written to %s\n", Shown, Argv[10]);
    }
    // With "near X0 Y0 Z0 X1 Y1 Z1" after the angle: the upright polygons whose box lies within that box - how they face,
    // how big they are, the most any of their places moved and how many of their outer edges are of each kind.
    if (Argc > 9 && strcmp(Argv[3], "near") == 0)
    {
        const double B[6] = { atof(Argv[4]), atof(Argv[5]), atof(Argv[6]), atof(Argv[7]), atof(Argv[8]), atof(Argv[9]) };
        // "all" after the box: the floors and ceilings too.
        const bool bAll = Argc > 10 && strcmp(Argv[10], "all") == 0;
        for (int p = 0; p < NumPolys; p++)
        {
            const FPolyIn& P = Polys[p];
            if ((!bAll && fabs(P.Nz) > 0.3) || P.Count < 3)
                continue;
            double X0 = 1e30, X1 = -1e30, Y0 = 1e30, Y1 = -1e30, Z0 = 1e30, Z1 = -1e30;
            for (int k = 0; k < P.Count; k++)
            {
                const FV3& Q = Pts[P.First + k];
                X0 = fmin(X0, Q.X); X1 = fmax(X1, Q.X);
                Y0 = fmin(Y0, Q.Y); Y1 = fmax(Y1, Q.Y);
                Z0 = fmin(Z0, Q.Z); Z1 = fmax(Z1, Q.Z);
            }
            if (X0 < B[0] || X1 > B[3] || Y0 < B[1] || Y1 > B[4] || Z0 < B[2] || Z1 > B[5])
                continue;
            double Most = 0.0;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                for (int k = 0; k < 3; k++)
                    Most = fmax(Most, RdxSphere::Length(R.Moved[R.Tri[t * 3 + k]] - R.Orig[R.Tri[t * 3 + k]]));
            const int* const K = &R.PolyEdgeKinds[p * 5];
            printf("  near: polygon %d, %d corners, x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f, normal %.3f %.3f %.3f, use %d, "
                   "the most %.2f; edges fixed %d, soft %d, crease %d, open %d\n", p, P.Count, X0, X1, Y0, Y1, Z0, Z1,
                   P.Nx, P.Ny, P.Nz, P.Use, Most, K[0], K[2], K[3], K[4]);
        }
    }
    // With "capped" after the angle: the polygons with a place held at the cap - where they are, how they face, how big
    // they are and the most any of their places moved - in cells of 512 units, the busiest first.
    if (Argc > 3 && strcmp(Argv[3], "capped") == 0)
        for (int p = 0; p < NumPolys; p++)
        {
            double Most = 0.0;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
                for (int k = 0; k < 3; k++)
                    Most = fmax(Most, RdxSphere::Length(R.Moved[R.Tri[t * 3 + k]] - R.Orig[R.Tri[t * 3 + k]]));
            if (Most < Cap - 1e-3)
                continue;
            const FPolyIn& P = Polys[p];
            double         X0 = 1e30, X1 = -1e30, Y0 = 1e30, Y1 = -1e30, Z0 = 1e30, Z1 = -1e30;
            for (int k = 0; k < P.Count; k++)
            {
                const FV3& Q = Pts[P.First + k];
                X0 = fmin(X0, Q.X); X1 = fmax(X1, Q.X);
                Y0 = fmin(Y0, Q.Y); Y1 = fmax(Y1, Q.Y);
                Z0 = fmin(Z0, Q.Z); Z1 = fmax(Z1, Q.Z);
            }
            printf("  capped: polygon %d, %d corners, centre %.0f %.0f %.0f, box %.0f x %.0f x %.0f, normal %.2f %.2f "
                   "%.2f, the most %.2f\n", p, P.Count, 0.5 * (X0 + X1), 0.5 * (Y0 + Y1), 0.5 * (Z0 + Z1), X1 - X0, Y1 - Y0,
                   Z1 - Z0, P.Nx, P.Ny, P.Nz, Most);
        }

    // Every edge of the moved polygons' triangles: how many triangles use it.
    const int Tris = R.Tri.Num / 3;
    struct FEdge
    {
        long long Key;
        int       Count;
    };
    int Size = 64;
    while (Size < Tris * 6)
        Size *= 2;
    TList<FEdge> Table;
    if (!Table.Resize(Size))
        return 1;
    for (int i = 0; i < Size; i++)
        Table[i].Key = -1;
    auto Use = [&](int A, int B)
    {
        const long long K = ((long long)((A < B) ? A : B) << 32) | (long long)(unsigned int)((A < B) ? B : A);
        unsigned long long H = (unsigned long long)K * 0x9E3779B97F4A7C15ull;
        int At = (int)(H >> 40) & (Size - 1);
        while (Table[At].Key != -1 && Table[At].Key != K)
            At = (At + 1) & (Size - 1);
        if (Table[At].Key == -1)
        {
            Table[At].Key   = K;
            Table[At].Count = 0;
        }
        Table[At].Count++;
    };
    // Turned over or flat: where it is now, against the way it faced where it was. A turned one is harmless where all of
    // it moved along its polygon's plane, by less than InPlane off it (its pictures are read where it is now - the
    // renderer's SphereReadAt - and it is drawn as what it lies over), or where it is a sliver of less than Sliver square
    // units; the rest show. With "detail" after the angle, the first of those
    // are said: their polygon, its size and the triangle's place.
    const double Sliver  = 0.1, InPlane = 0.05;
    const bool   bDetail = Argc > 3 && strcmp(Argv[3], "detail") == 0;
    int          Said    = 0;
    // LRC_DETAIL_BOX=x0,y0,z0,x1,y1,z1: with "detail", every showing one with its first corner in that box, not only
    // the first forty.
    double DetailBox[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    bool   bDetailBox   = false;
    if (const char* Db = getenv("LRC_DETAIL_BOX"))
        bDetailBox = sscanf(Db, "%lf,%lf,%lf,%lf,%lf,%lf", &DetailBox[0], &DetailBox[1], &DetailBox[2], &DetailBox[3],
                            &DetailBox[4], &DetailBox[5]) == 6;
    int          Turned = 0, TurnedInPlane = 0, TurnedSliver = 0, Flat = 0;
    int          TurnedByMoves = 0, TurnedByMovesWide = 0;   // turned by the bevel's own moves; of them a unit or more
    int          TurnedByMovesUncut = 0;   // of them, the rounding's own triangles - every corner a place it had
    int          TurnedByMovesOff = 0, TurnedByMovesOffWide = 0;   // of them, off their plane; of those a unit or more
    // How far the bevel itself moved a place (LRC_BEVEL's, from where the rounding or the cut put it); -1 no bevel's.
    const RdxSphere::TList<RdxSphere::FV3>& BevelBefore = RdxLevelRound::BevelDetail::LevelBevelTraceBefore();
    const int PlacesBeforeBevel = RdxLevelRound::BevelDetail::LevelBevelTracePlaces0();
    auto BevelOwn = [&](int w) {
        return (w < BevelBefore.Num) ? RdxSphere::Length(R.Moved[w] - BevelBefore[w]) : -1.0;
    };
    // LRC_TURNED_FILE=<file>: each triangle the bevel's own moves turned over, a line each - its polygon, its middle
    // after the moves, its area before and after, whether a corner left its plane, whether all three are the rounding's,
    // its three places.
    FILE* TurnedOut = getenv("LRC_TURNED_FILE") ? fopen(getenv("LRC_TURNED_FILE"), "w") : nullptr;
    for (int p = 0; p < NumPolys; p++)
        for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
        {
            const int A = R.Tri[t * 3], B = R.Tri[t * 3 + 1], C = R.Tri[t * 3 + 2];
            Use(A, B);
            Use(B, C);
            Use(C, A);
            const FV3    N  = RdxSphere::Cross(R.Moved[B] - R.Moved[A], R.Moved[C] - R.Moved[A]);
            const double L  = RdxSphere::Length(N);
            const double Lg = fmax(RdxSphere::Length(R.Moved[B] - R.Moved[A]),
                                   fmax(RdxSphere::Length(R.Moved[C] - R.Moved[B]), RdxSphere::Length(R.Moved[A] - R.Moved[C])));
            if (!(L > 1e-9 * Lg * Lg))
            {
                Flat++;
                continue;
            }
            const FV3 Was = RdxSphere::Cross(R.Orig[B] - R.Orig[A], R.Orig[C] - R.Orig[A]);
            // Turned by the bevel's own moves: against the way it faced where the rounding or the cut put its corners,
            // more than a sliver either way - what the places' moves turned, whatever the corners' texture places say.
            if (A < BevelBefore.Num && B < BevelBefore.Num && C < BevelBefore.Num)
            {
                const FV3 Then = RdxSphere::Cross(BevelBefore[B] - BevelBefore[A], BevelBefore[C] - BevelBefore[A]);
                if (RdxSphere::Dot(N, Then) < 0.0 && (0.5 * RdxSphere::Length(Then) >= Sliver || 0.5 * L >= Sliver))
                {
                    TurnedByMoves++;
                    TurnedByMovesWide += (0.5 * L >= 1.0) ? 1 : 0;
                    // Off its plane: a corner the moves took more than InPlane off the plane it lay in - one that stays
                    // in it is folded over within the surface, drawn as what it lies over.
                    const double ThenLength = RdxSphere::Length(Then);
                    bool         bOff       = false;
                    if (ThenLength > 0.0)
                    {
                        const FV3 Up = Then * (1.0 / ThenLength);
                        bOff = fabs(RdxSphere::Dot(R.Moved[A] - BevelBefore[A], Up)) > InPlane ||
                               fabs(RdxSphere::Dot(R.Moved[B] - BevelBefore[B], Up)) > InPlane ||
                               fabs(RdxSphere::Dot(R.Moved[C] - BevelBefore[C], Up)) > InPlane;
                    }
                    TurnedByMovesOff += bOff ? 1 : 0;
                    TurnedByMovesOffWide += (bOff && 0.5 * L >= 1.0) ? 1 : 0;
                    TurnedByMovesUncut += (A < PlacesBeforeBevel && B < PlacesBeforeBevel && C < PlacesBeforeBevel) ? 1 : 0;
                    if (TurnedOut)
                        fprintf(TurnedOut, "%d %.3f %.3f %.3f %.4f %.4f %d %d %d %d %d\n", p,
                                (R.Moved[A].X + R.Moved[B].X + R.Moved[C].X) / 3.0,
                                (R.Moved[A].Y + R.Moved[B].Y + R.Moved[C].Y) / 3.0,
                                (R.Moved[A].Z + R.Moved[B].Z + R.Moved[C].Z) / 3.0, 0.5 * RdxSphere::Length(Then),
                                0.5 * L, bOff ? 1 : 0,
                                (A < PlacesBeforeBevel && B < PlacesBeforeBevel && C < PlacesBeforeBevel) ? 1 : 0, A, B,
                                C);
                    // With "detail", the first dozen a square unit or more off their plane: their corners, before and after.
                    if (bDetail && bOff && 0.5 * L >= 1.0 && TurnedByMovesOffWide <= 12)
                    {
                        printf("  turned by moves: polygon %d (normal %.2f %.2f %.2f), %.3f square units before the moves, "
                               "%.3f after\n", p, Polys[p].Nx, Polys[p].Ny, Polys[p].Nz, 0.5 * RdxSphere::Length(Then),
                               0.5 * L);
                        const int Wc[3] = { A, B, C };
                        for (int k = 0; k < 3; k++)
                            printf("    place %d: %.3f %.3f %.3f -> %.3f %.3f %.3f\n", Wc[k], BevelBefore[Wc[k]].X,
                                   BevelBefore[Wc[k]].Y, BevelBefore[Wc[k]].Z, R.Moved[Wc[k]].X, R.Moved[Wc[k]].Y,
                                   R.Moved[Wc[k]].Z);
                    }
                }
            }
            if (RdxSphere::Dot(N, Was) >= 0.0)
                continue;
            Turned++;
            const FPolyIn& Pl = Polys[p];
            const FV3      Np = RdxSphere::V3(Pl.Nx, Pl.Ny, Pl.Nz);
            if (fabs(RdxSphere::Dot(R.Moved[A] - R.Orig[A], Np)) <= InPlane &&
                fabs(RdxSphere::Dot(R.Moved[B] - R.Orig[B], Np)) <= InPlane &&
                fabs(RdxSphere::Dot(R.Moved[C] - R.Orig[C], Np)) <= InPlane)
            {
                TurnedInPlane++;
                continue;
            }
            if (0.5 * RdxSphere::Length(Was) < Sliver && 0.5 * L < Sliver)
            {
                TurnedSliver++;
                continue;
            }
            const bool bInBox = bDetailBox && R.Orig[A].X >= DetailBox[0] && R.Orig[A].X <= DetailBox[3] &&
                                R.Orig[A].Y >= DetailBox[1] && R.Orig[A].Y <= DetailBox[4] &&
                                R.Orig[A].Z >= DetailBox[2] && R.Orig[A].Z <= DetailBox[5];
            if (bDetail && (bDetailBox ? bInBox : Said < 40))
            {
                Said++;
                const FPolyIn& P = Polys[p];
                double         X0 = 1e30, X1 = -1e30, Y0 = 1e30, Y1 = -1e30, Z0 = 1e30, Z1 = -1e30;
                for (int k = 0; k < P.Count; k++)
                {
                    const FV3& Q = Pts[P.First + k];
                    X0 = fmin(X0, Q.X); X1 = fmax(X1, Q.X);
                    Y0 = fmin(Y0, Q.Y); Y1 = fmax(Y1, Q.Y);
                    Z0 = fmin(Z0, Q.Z); Z1 = fmax(Z1, Q.Z);
                }
                printf("  turned: polygon %d (%d corners, box %.1f x %.1f x %.1f, normal %.2f %.2f %.2f) at %.1f %.1f %.1f; "
                       "the triangle was %.3f square units, is %.3f, its corners moved %.2f %.2f %.2f (places %d %d %d, the "
                       "bevel's own %.2f %.2f %.2f)\n",
                       p, P.Count, X1 - X0, Y1 - Y0, Z1 - Z0, P.Nx, P.Ny, P.Nz, R.Orig[A].X, R.Orig[A].Y, R.Orig[A].Z,
                       0.5 * RdxSphere::Length(Was), 0.5 * L, RdxSphere::Length(R.Moved[A] - R.Orig[A]),
                       RdxSphere::Length(R.Moved[B] - R.Orig[B]), RdxSphere::Length(R.Moved[C] - R.Orig[C]), A, B, C,
                       BevelOwn(A), BevelOwn(B), BevelOwn(C));
            }
        }
    const int TurnedShowing = Turned - TurnedInPlane - TurnedSliver;
    if (BevelBefore.Num > 0)
        printf("turned over by the bevel's own moves: %d (%d of them a square unit or more now, %d the rounding's own "
               "triangles); off their plane %d (%d a square unit or more)\n", TurnedByMoves, TurnedByMovesWide,
               TurnedByMovesUncut, TurnedByMovesOff, TurnedByMovesOffWide);
    // With "classify" after the angle: the showing ones sorted - the kind of face they lie in (a triangle piece, a quad,
    // none found), how many of their corners lie on their polygon's sides, how thin they were (the longest side over
    // the height), and how far apart their corners' moves are against the longest side.
    if (Argc > 3 && strcmp(Argv[3], "classify") == 0)
    {
        int ByKind[3] = {}, BySide[4] = {}, ByThin[4] = {}, BySpread[4] = {}, KindSide[3][4] = {};
        for (int p = 0; p < NumPolys; p++)
        {
            const FPolyIn& Pl = Polys[p];
            const FV3      Np = RdxSphere::V3(Pl.Nx, Pl.Ny, Pl.Nz);
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                const int Ix[3] = { R.Tri[t * 3], R.Tri[t * 3 + 1], R.Tri[t * 3 + 2] };
                const FV3 N   = RdxSphere::Cross(R.Moved[Ix[1]] - R.Moved[Ix[0]], R.Moved[Ix[2]] - R.Moved[Ix[0]]);
                const FV3 Was = RdxSphere::Cross(R.Orig[Ix[1]] - R.Orig[Ix[0]], R.Orig[Ix[2]] - R.Orig[Ix[0]]);
                bool bInPlane = true;
                for (int k = 0; k < 3; k++)
                    bInPlane = bInPlane && fabs(RdxSphere::Dot(R.Moved[Ix[k]] - R.Orig[Ix[k]], Np)) <= InPlane;
                if (!(RdxSphere::Dot(N, Was) < 0.0) || bInPlane ||
                    (0.5 * RdxSphere::Length(Was) < Sliver && 0.5 * RdxSphere::Length(N) < Sliver))
                    continue;
                // The face it lies in: its middle inside the face's corners, in the polygon's plane.
                const FV3 Mid = (R.Orig[Ix[0]] + R.Orig[Ix[1]] + R.Orig[Ix[2]]) * (1.0 / 3.0);
                int Kind = 2;
                for (int f = (p < R.PolyFaceFirst.Num) ? R.PolyFaceFirst[p] : 0;
                     p < R.PolyFaceCount.Num && f < R.PolyFaceFirst[p] + R.PolyFaceCount[p] && Kind == 2; f++)
                {
                    const int Corners = R.FaceKind[f] ? 4 : 3;
                    bool      bIn     = true;
                    for (int k = 0; k < Corners && bIn; k++)
                    {
                        const FV3 E = R.FaceCorner[f * 4 + (k + 1) % Corners] - R.FaceCorner[f * 4 + k];
                        const FV3 Q = Mid - R.FaceCorner[f * 4 + k];
                        bIn = RdxSphere::Dot(RdxSphere::Cross(E, Q), Np) >= -1e-6 * RdxSphere::Length(E);
                    }
                    if (!bIn)
                    {
                        bIn = true;
                        for (int k = 0; k < Corners && bIn; k++)
                        {
                            const FV3 E = R.FaceCorner[f * 4 + (k + 1) % Corners] - R.FaceCorner[f * 4 + k];
                            const FV3 Q = Mid - R.FaceCorner[f * 4 + k];
                            bIn = RdxSphere::Dot(RdxSphere::Cross(E, Q), Np) <= 1e-6 * RdxSphere::Length(E);
                        }
                    }
                    if (bIn)
                        Kind = R.FaceKind[f] ? 1 : 0;
                }
                // Its corners on the polygon's sides.
                int OnSide = 0;
                for (int k = 0; k < 3; k++)
                {
                    bool bOn = false;
                    for (int s = 0; s < Pl.Count && !bOn; s++)
                    {
                        const FV3    A  = Pts[Pl.First + s], B = Pts[Pl.First + (s + 1) % Pl.Count];
                        const FV3    AB = B - A;
                        const double L2 = RdxSphere::Dot(AB, AB);
                        const double U  = (L2 > 0.0) ? RdxSphere::Dot(R.Orig[Ix[k]] - A, AB) / L2 : 0.0;
                        if (U < -1e-6 || U > 1.0 + 1e-6)
                            continue;
                        bOn = RdxSphere::Length(R.Orig[Ix[k]] - (A + AB * U)) < 1e-3;
                    }
                    OnSide += bOn ? 1 : 0;
                }
                double Long = 0.0;
                for (int k = 0; k < 3; k++)
                    Long = fmax(Long, RdxSphere::Length(R.Orig[Ix[(k + 1) % 3]] - R.Orig[Ix[k]]));
                const double Height = RdxSphere::Length(Was) / fmax(Long, 1e-9);
                const double Thin   = Long / fmax(Height, 1e-9);
                double Spread = 0.0;
                for (int k = 0; k < 3; k++)
                    Spread = fmax(Spread, RdxSphere::Length((R.Moved[Ix[(k + 1) % 3]] - R.Orig[Ix[(k + 1) % 3]]) -
                                                            (R.Moved[Ix[k]] - R.Orig[Ix[k]])));
                const double Rel = Spread / fmax(Long, 1e-9);
                ByKind[Kind]++;
                BySide[OnSide]++;
                KindSide[Kind][OnSide]++;
                ByThin[(Thin < 4.0) ? 0 : (Thin < 10.0) ? 1 : (Thin < 30.0) ? 2 : 3]++;
                BySpread[(Rel < 0.25) ? 0 : (Rel < 1.0) ? 1 : (Rel < 4.0) ? 2 : 3]++;
            }
        }
        printf("classify: in a triangle piece %d, a quad %d, no face found %d\n", ByKind[0], ByKind[1], ByKind[2]);
        printf("classify: corners on the polygon's sides 0: %d, 1: %d, 2: %d, 3: %d\n", BySide[0], BySide[1], BySide[2],
               BySide[3]);
        for (int k = 0; k < 3; k++)
            printf("classify: %s - on sides 0: %d, 1: %d, 2: %d, 3: %d\n",
                   (k == 0) ? "triangle piece" : (k == 1) ? "quad" : "no face", KindSide[k][0], KindSide[k][1],
                   KindSide[k][2], KindSide[k][3]);
        printf("classify: thin (longest over height) under 4: %d, 4 to 10: %d, 10 to 30: %d, more: %d\n", ByThin[0],
               ByThin[1], ByThin[2], ByThin[3]);
        printf("classify: the corners' moves apart, over the longest side: under 1/4: %d, to 1: %d, to 4: %d, more: %d\n",
               BySpread[0], BySpread[1], BySpread[2], BySpread[3]);
    }
    // The showing ones by how thin their polygon's thinnest piece of the surface is: its longest side over its height.
    {
        int ByThin[4] = { 0, 0, 0, 0 };
        for (int p = 0; p < NumPolys; p++)
        {
            int Count = 0;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                const int A = R.Tri[t * 3], B = R.Tri[t * 3 + 1], C = R.Tri[t * 3 + 2];
                const FV3 N   = RdxSphere::Cross(R.Moved[B] - R.Moved[A], R.Moved[C] - R.Moved[A]);
                const FV3 Was = RdxSphere::Cross(R.Orig[B] - R.Orig[A], R.Orig[C] - R.Orig[A]);
                const FPolyIn& Pl = Polys[p];
                const FV3      Np = RdxSphere::V3(Pl.Nx, Pl.Ny, Pl.Nz);
                const bool bInPlane = fabs(RdxSphere::Dot(R.Moved[A] - R.Orig[A], Np)) <= InPlane &&
                                      fabs(RdxSphere::Dot(R.Moved[B] - R.Orig[B], Np)) <= InPlane &&
                                      fabs(RdxSphere::Dot(R.Moved[C] - R.Orig[C], Np)) <= InPlane;
                if (RdxSphere::Dot(N, Was) < 0.0 && !bInPlane &&
                    !(0.5 * RdxSphere::Length(Was) < Sliver && 0.5 * RdxSphere::Length(N) < Sliver))
                    Count++;
            }
            if (!Count)
                continue;
            double Thin = 0.0;
            for (int f = (p < R.PolyFaceFirst.Num) ? R.PolyFaceFirst[p] : 0;
                 p < R.PolyFaceCount.Num && f < R.PolyFaceFirst[p] + R.PolyFaceCount[p]; f++)
            {
                const FV3 P0 = R.FaceCorner[f * 4], P1 = R.FaceCorner[f * 4 + 1], P2 = R.FaceCorner[f * 4 + 2];
                const double L = fmax(RdxSphere::Length(P1 - P0), fmax(RdxSphere::Length(P2 - P1), RdxSphere::Length(P0 - P2)));
                const double H = RdxSphere::Length(RdxSphere::Cross(P1 - P0, P2 - P0)) / fmax(L, 1e-9);
                Thin = fmax(Thin, L / fmax(H, 1e-9));
            }
            ByThin[(Thin < 4.0) ? 0 : (Thin < 10.0) ? 1 : (Thin < 30.0) ? 2 : 3] += Count;
            // With "folds" after the angle: each polygon with showing ones - how many, its size and way, its pieces.
            if (Argc > 3 && strcmp(Argv[3], "folds") == 0)
            {
                const FPolyIn& P = Polys[p];
                double X0 = 1e30, X1 = -1e30, Y0 = 1e30, Y1 = -1e30, Z0 = 1e30, Z1 = -1e30;
                for (int k = 0; k < P.Count; k++)
                {
                    const FV3& Q = Pts[P.First + k];
                    X0 = fmin(X0, Q.X); X1 = fmax(X1, Q.X);
                    Y0 = fmin(Y0, Q.Y); Y1 = fmax(Y1, Q.Y);
                    Z0 = fmin(Z0, Q.Z); Z1 = fmax(Z1, Q.Z);
                }
                printf("  folds: polygon %d, %d showing, %d corners, x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f, normal %.2f %.2f "
                       "%.2f, %d pieces, the thinnest %.1f\n", p, Count, P.Count, X0, X1, Y0, Y1, Z0, Z1, P.Nx, P.Ny, P.Nz,
                       (p < R.PolyFaceCount.Num) ? R.PolyFaceCount[p] : 0, Thin);
            }
        }
        printf("showing, by their thinnest piece's length over its height: under 4 %d, 4 to 10 %d, 10 to 30 %d, more %d\n",
               ByThin[0], ByThin[1], ByThin[2], ByThin[3]);
    }
    // With "detail": the showing ones by how wide their polygon is - its area over its longest side.
    if (bDetail)
    {
        int Bucket[4] = { 0, 0, 0, 0 };
        for (int p = 0; p < NumPolys; p++)
        {
            int Count = 0;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                const int A = R.Tri[t * 3], B = R.Tri[t * 3 + 1], C = R.Tri[t * 3 + 2];
                const FV3 N   = RdxSphere::Cross(R.Moved[B] - R.Moved[A], R.Moved[C] - R.Moved[A]);
                const FV3 Was = RdxSphere::Cross(R.Orig[B] - R.Orig[A], R.Orig[C] - R.Orig[A]);
                const FPolyIn& Pl = Polys[p];
                const FV3      Np = RdxSphere::V3(Pl.Nx, Pl.Ny, Pl.Nz);
                const bool bInPlane = fabs(RdxSphere::Dot(R.Moved[A] - R.Orig[A], Np)) <= InPlane &&
                                      fabs(RdxSphere::Dot(R.Moved[B] - R.Orig[B], Np)) <= InPlane &&
                                      fabs(RdxSphere::Dot(R.Moved[C] - R.Orig[C], Np)) <= InPlane;
                if (RdxSphere::Dot(N, Was) < 0.0 && !bInPlane &&
                    !(0.5 * RdxSphere::Length(Was) < Sliver && 0.5 * RdxSphere::Length(N) < Sliver))
                    Count++;
            }
            if (!Count)
                continue;
            const FPolyIn& P = Polys[p];
            FV3            Nw = RdxSphere::V3(0.0, 0.0, 0.0);
            double         Longest = 0.0;
            for (int k = 0; k < P.Count; k++)
            {
                const FV3& Q0 = Pts[P.First + k];
                const FV3& Q1 = Pts[P.First + (k + 1) % P.Count];
                Nw            = Nw + RdxSphere::Cross(Q0, Q1);
                for (int j = 0; j < P.Count; j++)
                    Longest = fmax(Longest, RdxSphere::Length(Pts[P.First + j] - Q0));
            }
            const double Wide = (Longest > 0.0) ? 0.5 * RdxSphere::Length(Nw) / Longest : 0.0;
            Bucket[(Wide < 1.0) ? 0 : (Wide < 4.0) ? 1 : (Wide < 16.0) ? 2 : 3] += Count;
            static int WideSaid = 0;
            if (Wide >= 16.0 && WideSaid < 12)
            {
                WideSaid++;
                printf("  wide: polygon %d, %d corners, %.1f wide, %.0f long, %d showing; normal %.2f %.2f %.2f; first corner "
                       "%.1f %.1f %.1f\n", p, P.Count, Wide, Longest, Count, P.Nx, P.Ny, P.Nz, Pts[P.First].X,
                       Pts[P.First].Y, Pts[P.First].Z);
            }
        }
        printf("  showing, by their polygon's width: under 1 unit %d, 1 to 4 %d, 4 to 16 %d, wider %d\n", Bucket[0],
               Bucket[1], Bucket[2], Bucket[3]);
    }
    // The polygons left to the engine, with "detail": how many hold more than a square unit, and the first of those.
    if (bDetail)
    {
        int Big = 0;
        for (int i = 0; i < R.Left.Num; i++)
        {
            const FPolyIn& P = Polys[R.Left[i]];
            FV3            N = RdxSphere::V3(0.0, 0.0, 0.0);
            for (int k = 0; k < P.Count; k++)
                N = N + RdxSphere::Cross(Pts[P.First + k], Pts[P.First + (k + 1) % P.Count]);
            const double Area = 0.5 * RdxSphere::Length(N);
            if (Area <= 1.0)
                continue;
            if (Big++ < 20)
            {
                printf("  left: polygon %d, %d corners, %.2f square units:", R.Left[i], P.Count, Area);
                for (int k = 0; k < P.Count && k < 8; k++)
                    printf(" (%.2f %.2f %.2f)", Pts[P.First + k].X, Pts[P.First + k].Y, Pts[P.First + k].Z);
                printf("\n");
            }
        }
        printf("  left to the engine: %d, %d of them more than a square unit\n", R.Left.Num, Big);
    }
    // With "poly <n>" after the angle: polygon n's corners as the level has them, and its moved triangles where they were
    // and where they are.
    if (Argc > 4 && strcmp(Argv[3], "poly") == 0)
    {
        const int p = atoi(Argv[4]);
        if (p >= 0 && p < NumPolys)
        {
            const FPolyIn& P = Polys[p];
            printf("polygon %d: %d corners, normal %.4f %.4f %.4f\n", p, P.Count, P.Nx, P.Ny, P.Nz);
            for (int k = 0; k < P.Count; k++)
                printf("  corner %d: %.4f %.4f %.4f\n", k, Pts[P.First + k].X, Pts[P.First + k].Y, Pts[P.First + k].Z);
            // Its pieces of the rounded surface: corners, and each edge's bends (length and room) at its two ends.
            for (int f = (p < R.PolyFaceFirst.Num) ? R.PolyFaceFirst[p] : 0;
                 p < R.PolyFaceCount.Num && f < R.PolyFaceFirst[p] + R.PolyFaceCount[p]; f++)
            {
                printf("  piece %d:", f - R.PolyFaceFirst[p]);
                printf(" kind %d", (int)R.FaceKind[f]);
                for (int k = 0; k < 4; k++)
                    printf(" (%.2f %.2f %.2f)", R.FaceCorner[f * 4 + k].X, R.FaceCorner[f * 4 + k].Y, R.FaceCorner[f * 4 + k].Z);
                for (int e = 0; e < 4; e++)
                {
                    printf(" | edge %d:", e);
                    for (int k = 0; k < 4; k++)
                    {
                        const float* const S = &R.FaceTerm[f * RdxLevelRound::FaceTermFloats + (e * 4 + k) * 4];
                        printf(" %.2f/%.2f", sqrt(S[0] * S[0] + S[1] * S[1] + S[2] * S[2]), S[3]);
                    }
                }
                printf("\n");
            }
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                printf("  triangle %d:", t - R.PolyFirst[p]);
                for (int k = 0; k < 3; k++)
                {
                    const FV3& O = R.Orig[R.Tri[t * 3 + k]];
                    const FV3& M = R.Moved[R.Tri[t * 3 + k]];
                    printf(" (%.3f %.3f %.3f -> %.3f %.3f %.3f)", O.X, O.Y, O.Z, M.X, M.Y, M.Z);
                }
                printf("\n");
            }
        }
    }
    // LRC_SEAMS=1: a still wall's side along which a moved polygon's place lay (where it was, within a twentieth of a
    // unit, inside the side) and from which the place moved off - a seam the still wall does not follow, the rounding's
    // places not welded to its side: how many, the most off, and the first few.
    if (getenv("LRC_SEAMS"))
    {
        FCellMap Away;   // the moved polygons' places that moved, by cells of 16 units where they were
        std::vector<char> Taken(R.Orig.Num, 0);
        for (int p = 0; p < NumPolys && p < R.PolyTris.Num; p++)
            for (int t = R.PolyFirst[p] * 3; t < (R.PolyFirst[p] + R.PolyTris[p]) * 3; t++)
            {
                const int w = R.Tri[t];
                if (Taken[w] || RdxSphere::Length(R.Moved[w] - R.Orig[w]) <= 0.05)
                    continue;
                Taken[w] = 1;
                const FV3& O = R.Orig[w];
                Away.Add(RdxLevelRound::Detail::CellKey((long long)floor(O.X / 16.0), (long long)floor(O.Y / 16.0),
                                                        (long long)floor(O.Z / 16.0)),
                         w);
            }
        Away.Done();
        long long Places = 0, Sides = 0, OpenPlaces = 0, OpenSides = 0;
        double    Most   = 0.0, OpenMost = 0.0;
        // The open ones, the widest first said: how far, the still wall, the moved place and its polygons' first.
        std::vector<std::tuple<double, int, int>> Opens;
        for (int s = 0; s < NumPolys && s < R.RingCount.Num; s++)
        {
            if (!(Polys[s].Use & RdxSphere::Use_Wall) || (s < R.PolyTris.Num && R.PolyTris[s] > 0) || R.RingCount[s] < 3)
                continue;
            const int Rf = R.RingFirst[s], Rc = R.RingCount[s];
            // The wall's middle, for the way out of it across a side; its normal, for in front and behind.
            FV3 Mid = RdxSphere::V3(0.0, 0.0, 0.0);
            for (int i = 0; i < Rc; i++)
                Mid = Mid + R.Orig[R.Ring[Rf + i]] * (1.0 / Rc);
            const FV3 Ns = RdxSphere::V3(Polys[s].Nx, Polys[s].Ny, Polys[s].Nz);
            for (int i = 0; i < Rc; i++)
            {
                const FV3    U = R.Orig[R.Ring[Rf + i]], V = R.Orig[R.Ring[Rf + (i + 1) % Rc]];
                const FV3    D = V - U;
                const double L = RdxSphere::Length(D);
                if (!(L > 0.1))
                    continue;
                bool         bSide = false, bOpenSide = false;
                const double Cs = 16.0;
                const int    Steps = (int)(L / Cs) + 1;
                std::set<int> Seen;
                for (int k = 0; k <= Steps; k++)
                {
                    const FV3 Q = U + D * ((double)k / Steps);
                    for (int dz = -1; dz <= 1; dz++)
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++)
                                for (int w : Away.At(RdxLevelRound::Detail::CellKey(
                                         (long long)floor(Q.X / Cs) + dx, (long long)floor(Q.Y / Cs) + dy,
                                         (long long)floor(Q.Z / Cs) + dz)))
                                {
                                    if (!Seen.insert(w).second)
                                        continue;
                                    const double T = RdxSphere::Dot(R.Orig[w] - U, D) / (L * L);
                                    if (!(T * L > 0.05 && (1.0 - T) * L > 0.05))
                                        continue;
                                    if (RdxSphere::Length(R.Orig[w] - (U + D * T)) > 0.05)
                                        continue;
                                    const double Off = RdxSphere::Length(R.Moved[w] - R.Orig[w]);
                                    Places++;
                                    bSide = true;
                                    if (Off > Most)
                                        Most = Off;
                                    // Open: the place gone in front of the wall, or out of it across the side along its
                                    // plane - not behind it, where the wall hides it.
                                    const FV3    Go  = R.Moved[w] - R.Orig[w];
                                    const FV3    In0 = Mid - (U + D * T);
                                    FV3          In  = In0 - D * (RdxSphere::Dot(In0, D) / (L * L));
                                    In           = In - Ns * RdxSphere::Dot(In, Ns);
                                    const double Li  = RdxSphere::Length(In);
                                    const double Out = (Li > 0.0) ? -RdxSphere::Dot(Go, In) / Li : 0.0;
                                    const double Up  = RdxSphere::Dot(Go, Ns);
                                    const double Gap = (Up > Out) ? Up : Out;
                                    if (Gap > 0.05)
                                    {
                                        OpenPlaces++;
                                        bOpenSide = true;
                                        OpenMost  = (Gap > OpenMost) ? Gap : OpenMost;
                                        Opens.push_back(std::make_tuple(Gap, s, w));
                                    }
                                }
                }
                Sides += bSide ? 1 : 0;
                OpenSides += bOpenSide ? 1 : 0;
            }
        }
        printf("seams: still walls' sides a moved place left %lld, places %lld, the most off %.3f; of them open (the place in "
               "front of the wall or out of it across the side) sides %lld, places %lld, the most %.3f\n", Sides,
               Places, Most, OpenSides, OpenPlaces, OpenMost);
        std::sort(Opens.begin(), Opens.end(), [](const std::tuple<double, int, int>& X,
                                                 const std::tuple<double, int, int>& Y) { return std::get<0>(X) > std::get<0>(Y); });
        for (size_t k = 0; k < Opens.size() && k < 6; k++)
        {
            const int sp = std::get<1>(Opens[k]), w = std::get<2>(Opens[k]);
            int       Owner = -1;
            for (int p = 0; p < NumPolys && p < R.PolyTris.Num && Owner < 0; p++)
                for (int t = R.PolyFirst[p] * 3; t < (R.PolyFirst[p] + R.PolyTris[p]) * 3 && Owner < 0; t++)
                    if (R.Tri[t] == w)
                        Owner = p;
            printf("  open seam %.3f: still polygon %d (normal %.2f %.2f %.2f), place %d at %.1f %.1f %.1f moved to %.1f %.1f "
                   "%.1f, of polygon %d (normal %.2f %.2f %.2f)\n", std::get<0>(Opens[k]), sp, Polys[sp].Nx,
                   Polys[sp].Ny, Polys[sp].Nz, w, R.Orig[w].X, R.Orig[w].Y, R.Orig[w].Z, R.Moved[w].X, R.Moved[w].Y,
                   R.Moved[w].Z, Owner, (Owner >= 0) ? Polys[Owner].Nx : 0.0, (Owner >= 0) ? Polys[Owner].Ny : 0.0,
                   (Owner >= 0) ? Polys[Owner].Nz : 0.0);
        }
    }
    // An edge of one triangle only, where something moved: an opening.
    int Open = 0;
    double OpenMost = 0.0;
    for (int i = 0; i < Size; i++)
        if (Table[i].Key != -1 && Table[i].Count == 1)
        {
            const int    A = (int)(Table[i].Key >> 32), B = (int)(Table[i].Key & 0xFFFFFFFF);
            const double M = fmax(RdxSphere::Length(R.Moved[A] - R.Orig[A]), RdxSphere::Length(R.Moved[B] - R.Orig[B]));
            if (M > 1e-3)
            {
                Open++;
                OpenMost = fmax(OpenMost, M);
            }
        }

    // The light sides: along a side of the polygon's own, the way into it from there, the runs within the side.
    int BadSides = 0;
    for (int p = 0; p < R.PolySideCount.Num; p++)
        for (int s = R.PolySideFirst[p]; s < R.PolySideFirst[p] + R.PolySideCount[p]; s++)
        {
            const RdxSphere::FLightSide& Sd   = R.LightSides[s];
            const double                 Long = RdxSphere::Length(Sd.B - Sd.A);
            bool bGood = Long > 1e-6 && fabs(RdxSphere::Dot(Sd.Into, Sd.Into) - 1.0) < 1e-6 &&
                         fabs(RdxSphere::Dot(Sd.Into, Sd.B - Sd.A)) < 1e-6 * Long;
            for (int k = Sd.PartFirst; k < Sd.PartFirst + Sd.PartCount && bGood; k++)
            {
                const RdxSphere::FLightPart& Pt = R.LightParts[k];
                bGood = Pt.T0 > -1e-3 && Pt.T1 < Long + 1e-3 && Pt.T0 < Pt.T1 && Pt.Reach > 0.0 && Pt.Depth > 0.0 &&
                        Pt.Other >= 0 && Pt.Other < NumPolys && Pt.Other != p;
            }
            BadSides += bGood ? 0 : 1;
        }

    // The card's copy: the moved polygons' triangle corners, and their middles, moved as the CPU moves them.
    TList<unsigned int> Words;
    int    CardMiss = 0;
    double CardOff  = 0.0;
    // How many places the card moves off the CPU by more than a tenth and by more than a unit, and of those how many
    // because it took another polygon than the place's own.
    int    CardOffTenth = 0, CardOffUnit = 0, CardOtherTenth = 0, CardOtherUnit = 0;
    // The worst of them: where, its own polygon and the one the card took, and the two moves.
    double WorstOff = 0.0;
    FV3    WorstAt = RdxSphere::V3(0.0, 0.0, 0.0), WorstCpu = WorstAt, WorstGpu = WorstAt;
    int    WorstOwn = -1, WorstFound = -1;
    if (R.Touched > 0 && RdxLevelRound::PackRound(R, Words))
        for (int p = 0; p < NumPolys; p++)
            for (int f = (p < R.PolyFaceFirst.Num) ? R.PolyFaceFirst[p] : 0;
                 p < R.PolyFaceCount.Num && f < R.PolyFaceFirst[p] + R.PolyFaceCount[p]; f++)
            {
                const int Sides = R.FaceKind[f] ? 4 : 3;
                FV3       Mid   = RdxSphere::V3(0.0, 0.0, 0.0);
                for (int k = 0; k < Sides; k++)
                    Mid = Mid + R.FaceCorner[f * 4 + k] * (1.0 / Sides);
                FV3       Cpu = RdxSphere::V3(0.0, 0.0, 0.0), Gpu = Cpu;
                if (!RdxLevelRound::MoveOnPolygon(R, p, Mid, 2.0, Cpu))
                    continue;
                int       Found = -1;
                const FV3 Own   = RdxSphere::V3(Polys[p].Nx, Polys[p].Ny, Polys[p].Nz);
                if (!PackedMoveAt(Words, Mid, Gpu, &Found, &Own))
                {
                    CardMiss++;
                    continue;
                }
                const double Off = RdxSphere::Length(Cpu - Gpu);
                if (Off > WorstOff)
                {
                    WorstOff = Off;
                    WorstAt = Mid;
                    WorstOwn = p;
                    WorstFound = Found;
                    WorstCpu = Cpu;
                    WorstGpu = Gpu;
                }
                CardOffTenth += (Off > 0.1) ? 1 : 0;
                CardOffUnit += (Off > 1.0) ? 1 : 0;
                CardOtherTenth += (Off > 0.1 && Found != p) ? 1 : 0;
                CardOtherUnit += (Off > 1.0 && Found != p) ? 1 : 0;
                // Another polygon through the same place may be found first - it moves the place as well, by its own
                // patch; only a difference past the floats' own noise there counts.
                CardOff = fmax(CardOff, RdxSphere::Length(Cpu - Gpu));
            }

    // With LRC_FLOOR_NEED in the environment: how many of those triangles stand off the faces' curve - at their middle
    // and their sides' middles, by more than a twentieth - and how long finding that takes.
    if (getenv("LRC_FLOOR_NEED"))
    {
        const double NeedT0 = Now();
        int          NeedTris = 0, Off = 0, Corner = 0;
        for (int p = 0; p < NumPolys; p++)
        {
            if (p >= R.PolyRelief.Num || !R.PolyRelief[p] || R.PolyTris[p] <= 0 || R.PolyFaceCount[p] <= 0)
                continue;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                NeedTris++;
                FV3 O[3], M[3];
                for (int k = 0; k < 3; k++)
                {
                    O[k] = R.Orig[R.Tri[t * 3 + k]];
                    M[k] = R.Moved[R.Tri[t * 3 + k]] - O[k];
                }
                static const double Sh[4][3] = { { 1.0 / 3, 1.0 / 3, 1.0 / 3 }, { 0.5, 0.5, 0.0 }, { 0.0, 0.5, 0.5 },
                                                 { 0.5, 0.0, 0.5 } };
                bool bOff = false;
                for (int s = 0; s < 4 && !bOff; s++)
                {
                    const FV3 W   = O[0] * Sh[s][0] + O[1] * Sh[s][1] + O[2] * Sh[s][2];
                    const FV3 Lin = M[0] * Sh[s][0] + M[1] * Sh[s][1] + M[2] * Sh[s][2];
                    FV3       Fm  = RdxSphere::V3(0.0, 0.0, 0.0);
                    if (RdxLevelRound::MoveOnPolygon(R, p, W, 2.0, Fm))
                        bOff = RdxSphere::Length(Fm - Lin) > 0.05;
                }
                Off += bOff ? 1 : 0;
                Corner += (RdxSphere::Length(M[0]) > 1e-4 || RdxSphere::Length(M[1]) > 1e-4 ||
                           RdxSphere::Length(M[2]) > 1e-4) ? 1 : 0;
            }
        }
        printf("the floor's need: %d triangles, %d of them with a corner moved, %d off the curve by more than a twentieth; "
               "%.0f ms\n", NeedTris, Corner, Off, (Now() - NeedT0) * 1000.0);
        // Whole polygons: one is taken where any of its triangles stands off the curve by more than the bound, on a grid
        // of quarters - for bounds of a tenth, a quarter, a half and one.
        static const double Bound[4] = { 0.1, 0.25, 0.5, 1.0 };
        int          PolysOver[4] = {}, TrisOver[4] = {}, PolysAll = 0;
        const double PolyT0 = Now();
        for (int p = 0; p < NumPolys; p++)
        {
            if (p >= R.PolyRelief.Num || !R.PolyRelief[p] || R.PolyTris[p] <= 0 || R.PolyFaceCount[p] <= 0)
                continue;
            PolysAll++;
            double Most = 0.0;
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p] && Most <= Bound[3]; t++)
            {
                FV3 O[3], M[3];
                for (int k = 0; k < 3; k++)
                {
                    O[k] = R.Orig[R.Tri[t * 3 + k]];
                    M[k] = R.Moved[R.Tri[t * 3 + k]] - O[k];
                }
                for (int i = 0; i <= 4 && Most <= Bound[3]; i++)
                    for (int j = 0; i + j <= 4 && Most <= Bound[3]; j++)
                    {
                        const double B1 = i / 4.0, B2 = j / 4.0, B0 = 1.0 - B1 - B2;
                        FV3          Fm = RdxSphere::V3(0.0, 0.0, 0.0);
                        if (RdxLevelRound::MoveOnPolygon(R, p, O[0] * B0 + O[1] * B1 + O[2] * B2, 2.0, Fm))
                            Most = fmax(Most, RdxSphere::Length(Fm - (M[0] * B0 + M[1] * B1 + M[2] * B2)));
                    }
            }
            for (int b = 0; b < 4; b++)
                if (Most > Bound[b])
                {
                    PolysOver[b]++;
                    TrisOver[b] += R.PolyTris[p];
                }
        }
        printf("the floor by whole polygons (%d of them, %.0f ms): over a tenth %d with %d triangles, a quarter %d with %d, "
               "a half %d with %d, one %d with %d\n", PolysAll, (Now() - PolyT0) * 1000.0, PolysOver[0], TrisOver[0],
               PolysOver[1], TrisOver[1], PolysOver[2], TrisOver[2], PolysOver[3], TrisOver[3]);
    }
    // The relief's floor on the card (PackedPieceMoveAt): each triangle of a moved polygon the relief may be drawn on,
    // sampled on a grid where it was, moved as the card moves it held against where the triangle is drawn - the relief
    // must lie on the surface drawn under it. How many the card found on none, on another polygon's triangle, and off
    // by more than a hundredth and a tenth.
    if (Words.Num > 0)
    {
        int    FloorPolys = 0, FloorTris = 0, FloorFlat = 0, FloorSamples = 0, FloorMiss = 0, FloorOther = 0,
               FloorOff[2] = {};
        double FloorMost = 0.0;
        // Of them, those well inside their triangle - a quarter of its shares in from every side, off any side another
        // polygon's triangles may meet at a corner the triangle has none at.
        int    InSamples = 0, InMiss = 0, InOff[2] = {};
        double InMost = 0.0;
        // The floor's normal there (what the relief is lifted along) against the drawn triangle's own, inside the
        // polygon's own triangles: how many samples, how many turned off the plane by more than a degree, how many off
        // the triangle by more than 5, 15 and 30 degrees, the most and where.
        int    NormSamples = 0, NormTurned = 0, NormOver[3] = {}, NormOverturned = 0;
        double NormMost = 0.0;
        FV3    NormWorstW = RdxSphere::V3(0.0, 0.0, 0.0);
        int    NormWorstP = -1;
        FV3    MissW = RdxSphere::V3(0.0, 0.0, 0.0);
        int    MissP = -1;
        // The worst: where, its polygon and the one the card took, the drawn move and the card's.
        FV3 FloorWorstW = RdxSphere::V3(0.0, 0.0, 0.0), FloorWorstDrawn = FloorWorstW, FloorWorstCard = FloorWorstW;
        int FloorWorstP = -1, FloorWorstFound = -1, FloorWorstT = -1;
        for (int p = 0; p < NumPolys; p++)
        {
            if (p >= R.PolyRelief.Num || !R.PolyRelief[p] || R.PolyTris[p] <= 0 || R.PolyFaceCount[p] <= 0)
                continue;
            FloorPolys++;
            const FV3 Np = RdxSphere::V3(Polys[p].Nx, Polys[p].Ny, Polys[p].Nz);
            for (int t = R.PolyFirst[p]; t < R.PolyFirst[p] + R.PolyTris[p]; t++)
            {
                FloorTris++;
                const FV3 O0 = R.Orig[R.Tri[t * 3]], O1 = R.Orig[R.Tri[t * 3 + 1]], O2 = R.Orig[R.Tri[t * 3 + 2]];
                const FV3 M0 = R.Moved[R.Tri[t * 3]], M1 = R.Moved[R.Tri[t * 3 + 1]], M2 = R.Moved[R.Tri[t * 3 + 2]];
                // A triangle of no area where it was - one that opens as its corners move, where a corner of the
                // triangles beside lies on another's side - has nothing of the relief's own on it.
                const FV3 Ar = RdxSphere::Cross(O1 - O0, O2 - O0);
                if (RdxSphere::Dot(Ar, Ar) < 1e-6 * RdxSphere::Dot(O1 - O0, O1 - O0) * RdxSphere::Dot(O2 - O0, O2 - O0))
                {
                    FloorFlat++;
                    continue;
                }
                for (int i = 0; i <= 4; i++)
                    for (int j = 0; i + j <= 4; j++)
                    {
                        const double B1 = i / 4.0, B2 = j / 4.0, B0 = 1.0 - B1 - B2;
                        // Where the relief's patch has it: on the polygon's own plane, which a corner of a
                        // neighbour's taken in may lie a little off.
                        const FV3    Wt  = O0 * B0 + O1 * B1 + O2 * B2;
                        const FV3    W   = Wt - Np * (RdxSphere::Dot(Np, Wt) - Polys[p].W);
                        const FV3    Drawn = M0 * B0 + M1 * B1 + M2 * B2 + (W - Wt);
                        FV3          Mv  = RdxSphere::V3(0.0, 0.0, 0.0), Nc = Np;
                        int          Found = -1;
                        FloorSamples++;
                        const bool bIn = i >= 1 && j >= 1 && i + j <= 3;
                        InSamples += bIn ? 1 : 0;
                        if (!RdxLevelRound::PackedPieceMoveAt(Words, W, Np, Mv, &Found, &Nc))
                        {
                            FloorMiss++;
                            InMiss += bIn ? 1 : 0;
                            if (bIn || MissP < 0)
                            {
                                MissW = W;
                                MissP = p;
                            }
                            continue;
                        }
                        FloorOther += (Found != p) ? 1 : 0;
                        FV3          Nt = RdxSphere::Cross(M1 - M0, M2 - M0);
                        const double Lt = RdxSphere::Length(Nt);
                        if (bIn && Found == p && Lt > 1e-9)
                            Nt = Nt * (((RdxSphere::Dot(Ar, Np) < 0.0) ? -1.0 : 1.0) / Lt);
                        // A triangle the rounding turned over faces back: its own normal says nothing of the surface's
                        // there.
                        NormOverturned += (bIn && Found == p && Lt > 1e-9 && RdxSphere::Dot(Nt, Np) <= 0.0) ? 1 : 0;
                        if (bIn && Found == p && Lt > 1e-9 && RdxSphere::Dot(Nt, Np) > 0.0)
                        {
                            const double Deg = acos(fmax(-1.0, fmin(1.0, RdxSphere::Dot(Nt, Nc)))) * 57.29578;
                            NormSamples++;
                            NormTurned += (RdxSphere::Dot(Nc, Np) < cos(1.0 / 57.29578)) ? 1 : 0;
                            NormOver[0] += (Deg > 5.0) ? 1 : 0;
                            NormOver[1] += (Deg > 15.0) ? 1 : 0;
                            NormOver[2] += (Deg > 30.0) ? 1 : 0;
                            if (Deg > NormMost)
                            {
                                NormMost   = Deg;
                                NormWorstW = W;
                                NormWorstP = p;
                            }
                        }
                        const double Off = RdxSphere::Length(W + Mv - Drawn);
                        FloorOff[0] += (Off > 0.01) ? 1 : 0;
                        FloorOff[1] += (Off > 0.1) ? 1 : 0;
                        if (bIn)
                        {
                            InOff[0] += (Off > 0.01) ? 1 : 0;
                            InOff[1] += (Off > 0.1) ? 1 : 0;
                            InMost = fmax(InMost, Off);
                        }
                        if (Off > FloorMost)
                        {
                            FloorWorstW     = W;
                            FloorWorstDrawn = Drawn - W;
                            FloorWorstCard  = Mv;
                            FloorWorstP     = p;
                            FloorWorstFound = Found;
                            FloorWorstT     = t - R.PolyFirst[p];
                        }
                        FloorMost = fmax(FloorMost, Off);
                    }
            }
        }
        printf("the relief's floor on the card: %d polygons, %d triangles (%d of no area where they were), the field %.1f "
               "MB (the floor %.1f MB of it); %d samples, on none %d, on another polygon's %d, off the drawn triangle by more than 0.01: %d, 0.1: %d "
               "(the most %.4f); inside the triangles %d samples, on none %d, off by more than 0.01: %d, 0.1: %d (the most "
               "%.4f)\n", FloorPolys, FloorTris, FloorFlat, Words.Num * 4.0 / 1048576.0,
               (Words.Num - (int)Words[15]) * 4.0 / 1048576.0, FloorSamples, FloorMiss, FloorOther,
               FloorOff[0], FloorOff[1], FloorMost, InSamples, InMiss, InOff[0], InOff[1], InMost);
        printf("  its normal inside the polygons' own triangles: %d samples (and %d on triangles turned over), turned off "
               "the plane by over a degree %d; off the drawn triangle by more than 5 degrees %d, 15 %d, 30 %d (the most %.1f, "
               "at %.2f %.2f %.2f on polygon %d)\n", NormSamples, NormOverturned, NormTurned, NormOver[0], NormOver[1],
               NormOver[2], NormMost, NormWorstW.X, NormWorstW.Y, NormWorstW.Z, NormWorstP);
        if (MissP >= 0)
            printf("  a place on none: %.2f %.2f %.2f on polygon %d (normal %.2f %.2f %.2f, plane %.2f; its record %s, "
                   "%d triangles)\n", MissW.X, MissW.Y, MissW.Z, MissP, Polys[MissP].Nx, Polys[MissP].Ny, Polys[MissP].Nz,
                   Polys[MissP].W, Words[(int)Words[15] + MissP] ? "made" : "NOT made", R.PolyTris[MissP]);
        // Why that place was on none: the polygons the floor's plan grid lists in its cell, each one's plane's distance,
        // and the best its triangles do.
        if (MissP >= 0)
        {
            auto Fw = [&](unsigned int i)
            {
                float V;
                memcpy(&V, &Words[(int)i], sizeof(V));
                return V;
            };
            const unsigned int GW = Words[16], GH = Words[17];
            const int Cx = (int)floorf(((float)MissW.X - Fw(18)) / Fw(4)), Cy = (int)floorf(((float)MissW.Y - Fw(19)) / Fw(4));
            printf("    its cell %d %d of %u x %u", Cx, Cy, GW, GH);
            if (Cx >= 0 && Cy >= 0 && Cx < (int)GW && Cy < (int)GH)
            {
                const unsigned int C = Words[20] + 2 * ((unsigned int)Cy * GW + (unsigned int)Cx);
                printf(", %u polygons:", Words[C + 1]);
                for (unsigned int i = 0; i < Words[C + 1] && i < 12; i++)
                {
                    const unsigned int P  = Words[Words[C] + i];
                    const unsigned int At = Words[Words[15] + P];
                    const float Off = fabsf(Fw(At) * (float)MissW.X + Fw(At + 1) * (float)MissW.Y + Fw(At + 2) * (float)MissW.Z -
                                            Fw(At + 3));
                    printf(" %u (off %.3f, normal %.2f %.2f %.2f)", P, Off, Fw(At), Fw(At + 1), Fw(At + 2));
                }
            }
            printf("\n");
        }
        if (MissP >= 0)
            for (int t = R.PolyFirst[MissP]; t < R.PolyFirst[MissP] + R.PolyTris[MissP]; t++)
            {
                const FV3 A = R.Orig[R.Tri[t * 3]], B = R.Orig[R.Tri[t * 3 + 1]], C = R.Orig[R.Tri[t * 3 + 2]];
                const FV3 X = RdxSphere::Cross(B - A, C - A);
                printf("    its triangle %d: %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f, twice its area %.3f along %.3f %.3f "
                       "%.3f\n", t - R.PolyFirst[MissP], A.X, A.Y, A.Z, B.X, B.Y, B.Z, C.X, C.Y, C.Z, RdxSphere::Length(X),
                       X.X, X.Y, X.Z);
            }
        // The polygons the floor has no record of - more places, triangles or items than sixteen bits name.
        int Unmade = 0, UnmadeTris = 0, UnmadeMost = 0;
        for (int p = 0; p < NumPolys; p++)
            if (p < R.PolyRelief.Num && R.PolyRelief[p] && R.PolyTris[p] > 0 && R.PolyFaceCount[p] > 0 &&
                Words[(int)Words[15] + p] == 0)
            {
                Unmade++;
                UnmadeTris += R.PolyTris[p];
                UnmadeMost = (R.PolyTris[p] > UnmadeMost) ? R.PolyTris[p] : UnmadeMost;
            }
        printf("  polygons with no record of the floor: %d, with %d triangles (the most %d)\n", Unmade, UnmadeTris,
               UnmadeMost);
        if (FloorWorstP >= 0)
        {
            auto Off = [&](int q)
            {
                return (q < 0) ? -1.0
                               : fabs(Polys[q].Nx * FloorWorstW.X + Polys[q].Ny * FloorWorstW.Y + Polys[q].Nz * FloorWorstW.Z - Polys[q].W);
            };
            printf("  the worst: at %.2f %.2f %.2f on polygon %d (triangle %d, normal %.2f %.2f %.2f), the card took "
                   "polygon %d (normal %.2f %.2f %.2f, off its plane %.3f, the own %.3f); drawn %.3f %.3f %.3f, card %.3f "
                   "%.3f %.3f\n", FloorWorstW.X, FloorWorstW.Y, FloorWorstW.Z, FloorWorstP, FloorWorstT, Polys[FloorWorstP].Nx, Polys[FloorWorstP].Ny,
                   Polys[FloorWorstP].Nz, FloorWorstFound, (FloorWorstFound >= 0) ? Polys[FloorWorstFound].Nx : 0.0,
                   (FloorWorstFound >= 0) ? Polys[FloorWorstFound].Ny : 0.0, (FloorWorstFound >= 0) ? Polys[FloorWorstFound].Nz : 0.0,
                   Off(FloorWorstFound), Off(FloorWorstP), FloorWorstDrawn.X, FloorWorstDrawn.Y, FloorWorstDrawn.Z, FloorWorstCard.X, FloorWorstCard.Y,
                   FloorWorstCard.Z);
            // Every triangle of its polygon the place lies on, where it was: its corners and how they moved.
            const int q = FloorWorstP;
            for (int t = R.PolyFirst[q]; t < R.PolyFirst[q] + R.PolyTris[q]; t++)
            {
                const FV3 O0 = R.Orig[R.Tri[t * 3]], O1 = R.Orig[R.Tri[t * 3 + 1]], O2 = R.Orig[R.Tri[t * 3 + 2]];
                const FV3 E1 = O1 - O0, E2 = O2 - O0, Q = FloorWorstW - O0;
                const double D00 = RdxSphere::Dot(E1, E1), D01 = RdxSphere::Dot(E1, E2), D11 = RdxSphere::Dot(E2, E2);
                const double Den = D00 * D11 - D01 * D01;
                if (!(fabs(Den) > 1e-12))
                    continue;
                const double D20 = RdxSphere::Dot(Q, E1), D21 = RdxSphere::Dot(Q, E2);
                const double B1 = (D11 * D20 - D01 * D21) / Den, B2 = (D00 * D21 - D01 * D20) / Den, B0 = 1.0 - B1 - B2;
                if (fmin(B0, fmin(B1, B2)) < -1e-3)
                    continue;
                printf("    on triangle %d (shares %.3f %.3f %.3f):", t - R.PolyFirst[q], B0, B1, B2);
                for (int k = 0; k < 3; k++)
                {
                    const int v = R.Tri[t * 3 + k];
                    const FV3 M = R.Moved[v] - R.Orig[v];
                    printf("  place %d at %.2f %.2f %.2f moved %.3f %.3f %.3f", v, R.Orig[v].X, R.Orig[v].Y, R.Orig[v].Z,
                           M.X, M.Y, M.Z);
                }
                printf("\n");
            }
        }
    }
    // With "probe X0 Y0 Z0 X1 Y1 Z1 N P..." after the angle: N places along the line from the first point to the second,
    // each moved as the CPU moves it with each of the polygons P (MoveOnPolygon, where it lies on one) and as the card's
    // copy finds and moves it (PackedMoveAt, the plan grid's polygons).
    if (Argc > 11 && strcmp(Argv[3], "probe") == 0)
    {
        const FV3 A = RdxSphere::V3(atof(Argv[4]), atof(Argv[5]), atof(Argv[6]));
        const FV3 B = RdxSphere::V3(atof(Argv[7]), atof(Argv[8]), atof(Argv[9]));
        const int N = atoi(Argv[10]);
        for (int i = 0; i <= N; i++)
        {
            const FV3 W = A + (B - A) * ((double)i / (double)(N > 0 ? N : 1));
            printf("probe %.2f %.2f %.2f:", W.X, W.Y, W.Z);
            for (int k = 11; k < Argc; k++)
            {
                const int p = atoi(Argv[k]);
                FV3       Cpu = RdxSphere::V3(0.0, 0.0, 0.0);
                if (p >= 0 && p < NumPolys && RdxLevelRound::MoveOnPolygon(R, p, W, 2.0, Cpu))
                    printf("  cpu %d (%.3f %.3f %.3f)", p, Cpu.X, Cpu.Y, Cpu.Z);
            }
            FV3 Gpu = RdxSphere::V3(0.0, 0.0, 0.0);
            if (Words.Num > 0 && PackedMoveAt(Words, W, Gpu))
                printf("  card (%.3f %.3f %.3f)", Gpu.X, Gpu.Y, Gpu.Z);
            else
                printf("  card none");
            printf("\n");
        }
    }
    // With "still" after the angle: the polygons drawn as they are (none of their places moved), sampled on a grid, the
    // card's field asked by the plan grid - as SolidPass.hlsl's SphereFaceAt asks it for a relief run of more polygons
    // than it lists: where it finds another polygon's face and moves the place, the relief would move off its flat
    // polygon. With a box after it (X0 Y0 Z0 X1 Y1 Z1), only the polygons whose middle lies in it, each one said.
    if (Argc > 3 && strcmp(Argv[3], "still") == 0)
    {
        const bool   bBox  = Argc > 9;
        const double Bx[6] = { bBox ? atof(Argv[4]) : 0.0, bBox ? atof(Argv[5]) : 0.0, bBox ? atof(Argv[6]) : 0.0,
                               bBox ? atof(Argv[7]) : 0.0, bBox ? atof(Argv[8]) : 0.0, bBox ? atof(Argv[9]) : 0.0 };
        int    Polys0 = 0, Moved0 = 0, Samples = 0, Off[3] = {};
        double Most = 0.0;
        for (int q = 0; q < NumPolys; q++)
        {
            if (Polys[q].Use == 0 || (q < R.PolyTris.Num && R.PolyTris[q] > 0) || Polys[q].Count < 3)
                continue;
            const FV3 Np = RdxSphere::V3(Polys[q].Nx, Polys[q].Ny, Polys[q].Nz);
            FV3       Mid = RdxSphere::V3(0.0, 0.0, 0.0);
            for (int k = 0; k < Polys[q].Count; k++)
                Mid = Mid + Pts[Polys[q].First + k] * (1.0 / Polys[q].Count);
            if (bBox && (Mid.X < Bx[0] || Mid.Y < Bx[1] || Mid.Z < Bx[2] || Mid.X > Bx[3] || Mid.Y > Bx[4] || Mid.Z > Bx[5]))
                continue;
            Polys0++;
            double HereMost = 0.0, HereN = 0.0;
            int    HereOff = 0, HereSamples = 0;
            // A fan from the first corner, each triangle on a grid of sixteenths.
            const FV3 A = Pts[Polys[q].First];
            for (int k = 1; k + 1 < Polys[q].Count; k++)
            {
                const FV3 B = Pts[Polys[q].First + k], C = Pts[Polys[q].First + k + 1];
                for (int i = 0; i <= 16; i++)
                    for (int j = 0; i + j <= 16; j++)
                    {
                        const FV3 W  = A * (1.0 - (i + j) / 16.0) + B * (i / 16.0) + C * (j / 16.0);
                        FV3       Mv = RdxSphere::V3(0.0, 0.0, 0.0);
                        Samples++;
                        HereSamples++;
                        if (!PackedMoveAt(Words, W, Mv))
                            continue;
                        const double L = RdxSphere::Length(Mv);
                        Off[0] += (L > 0.1) ? 1 : 0;
                        Off[1] += (L > 0.5) ? 1 : 0;
                        Off[2] += (L > 2.0) ? 1 : 0;
                        HereOff += (L > 0.1) ? 1 : 0;
                        if (L > HereMost)
                        {
                            HereMost = L;
                            HereN    = RdxSphere::Dot(Mv, Np);
                        }
                    }
            }
            Moved0 += (HereMost > 0.1) ? 1 : 0;
            Most = fmax(Most, HereMost);
            if (bBox)
                printf("  still: polygon %d, middle %.1f %.1f %.1f, normal %.2f %.2f %.2f: %d of %d samples moved by the "
                       "field more than 0.1, the most %.3f (%.3f along its normal)\n", q, Mid.X, Mid.Y, Mid.Z, Np.X, Np.Y,
                       Np.Z, HereOff, HereSamples, HereMost, HereN);
        }
        printf("still: %d polygons drawn as they are, %d of them moved by the field somewhere; %d samples, moved more than "
               "0.1: %d, 0.5: %d, 2: %d; the most %.3f\n", Polys0, Moved0, Samples, Off[0], Off[1], Off[2], Most);
    }
    // With "cover" after the angle: the drawn pieces against the card's field - each moved polygon's triangles sampled on
    // a grid, the flat triangle through their moved corners held against the field's move at the same place of the
    // level, along the polygon's normal. Where a piece stands in front of the surface the relief's patches follow, the
    // relief takes it for something standing over it (SolidPass.hlsl's ReliefClear).
    if (Argc > 3 && strcmp(Argv[3], "cover") == 0)
    {
        // With a box after it (X0 Y0 Z0 X1 Y1 Z1): only the polygons whose middle lies in it, each one said.
        const bool   bBox   = Argc > 9;
        const double Bx[6]  = { bBox ? atof(Argv[4]) : 0.0, bBox ? atof(Argv[5]) : 0.0, bBox ? atof(Argv[6]) : 0.0,
                                bBox ? atof(Argv[7]) : 0.0, bBox ? atof(Argv[8]) : 0.0, bBox ? atof(Argv[9]) : 0.0 };
        const int Steps = 8;
        int    Samples = 0, Front[4] = {}, Behind = 0, NoFace = 0, NoFaceMoved = 0;
        double Most = 0.0;
        struct FWorst { double Dn; int Poly, Tri, I, J; FV3 W; };
        FWorst Top[24];
        int    TopNum = 0;
        for (int q = 0; q < NumPolys; q++)
        {
            if (q >= R.PolyTris.Num || R.PolyTris[q] <= 0)
                continue;
            const FV3 Np = RdxSphere::V3(Polys[q].Nx, Polys[q].Ny, Polys[q].Nz);
            FV3       Mid = RdxSphere::V3(0.0, 0.0, 0.0);
            for (int k = 0; k < Polys[q].Count; k++)
                Mid = Mid + Pts[Polys[q].First + k] * (1.0 / Polys[q].Count);
            if (bBox && (Mid.X < Bx[0] || Mid.Y < Bx[1] || Mid.Z < Bx[2] || Mid.X > Bx[3] || Mid.Y > Bx[4] || Mid.Z > Bx[5]))
                continue;
            int    HereFront = 0, HereNoFace = 0, HereSamples = 0;
            double HereBehind = 0.0;
            FWorst Here = { 0.0, -1, -1, 0, 0, RdxSphere::V3(0.0, 0.0, 0.0) };
            for (int t = R.PolyFirst[q]; t < R.PolyFirst[q] + R.PolyTris[q]; t++)
            {
                const FV3 O0 = R.Orig[R.Tri[t * 3]], O1 = R.Orig[R.Tri[t * 3 + 1]], O2 = R.Orig[R.Tri[t * 3 + 2]];
                const FV3 M0 = R.Moved[R.Tri[t * 3]], M1 = R.Moved[R.Tri[t * 3 + 1]], M2 = R.Moved[R.Tri[t * 3 + 2]];
                for (int i = 0; i <= Steps; i++)
                    for (int j = 0; i + j <= Steps; j++)
                    {
                        const double B1 = (double)i / Steps, B2 = (double)j / Steps, B0 = 1.0 - B1 - B2;
                        const FV3    W   = O0 * B0 + O1 * B1 + O2 * B2;
                        const FV3    Cpu = M0 * B0 + M1 * B1 + M2 * B2;
                        FV3          Mv  = RdxSphere::V3(0.0, 0.0, 0.0);
                        Samples++;
                        HereSamples++;
                        if (!RdxLevelRound::MoveOnPolygon(R, q, W, 2.0, Mv))
                        {
                            NoFace++;
                            HereNoFace++;
                            NoFaceMoved += (RdxSphere::Length(Cpu - W) > 0.1) ? 1 : 0;
                            continue;
                        }
                        const double Dn = RdxSphere::Dot(Cpu - (W + Mv), Np);
                        Front[0] += (Dn > 0.1) ? 1 : 0;
                        Front[1] += (Dn > 0.25) ? 1 : 0;
                        Front[2] += (Dn > 0.5) ? 1 : 0;
                        Front[3] += (Dn > 1.0) ? 1 : 0;
                        Behind += (Dn < -0.25) ? 1 : 0;
                        HereFront += (Dn > 0.1) ? 1 : 0;
                        HereBehind = fmin(HereBehind, Dn);
                        Most = fmax(Most, Dn);
                        if (Dn > Here.Dn)
                            Here = { Dn, q, t - R.PolyFirst[q], i, j, W };
                    }
            }
            if (bBox)
            {
                printf("  box: polygon %d, middle %.1f %.1f %.1f, normal %.2f %.2f %.2f, %d corners, %d triangles, %d faces; %d "
                       "samples: %d in front by more than 0.1, the most %.3f (triangle %d at %d/%d), %d on no face, the most "
                       "behind %.3f\n", q, Mid.X, Mid.Y, Mid.Z, Np.X, Np.Y, Np.Z, Polys[q].Count, R.PolyTris[q],
                       (q < R.PolyFaceCount.Num) ? R.PolyFaceCount[q] : 0, HereSamples, HereFront, Here.Dn, Here.Tri, Here.I,
                       Here.J, HereNoFace, HereBehind);
                continue;
            }
            if (Here.Poly < 0 || Here.Dn <= 0.25)
                continue;
            if (TopNum == 24 && Top[23].Dn >= Here.Dn)
                continue;
            int k = (TopNum < 24) ? TopNum++ : 23;
            Top[k] = Here;
            for (; k > 0 && Top[k - 1].Dn < Top[k].Dn; k--)
            {
                const FWorst Sw = Top[k - 1];
                Top[k - 1]      = Top[k];
                Top[k]          = Sw;
            }
        }
        printf("cover: %d samples of the drawn pieces; in front of the field by more than 0.1: %d, 0.25: %d, 0.5: %d, 1: %d "
               "(the most %.3f); behind by more than 0.25: %d; on no face of their polygon: %d, %d of them moved\n", Samples,
               Front[0], Front[1], Front[2], Front[3], Most, Behind, NoFace, NoFaceMoved);
        for (int k = 0; k < TopNum; k++)
        {
            printf("  polygon %d, triangle %d at shares %d/%d of %d: %.3f in front, at %.1f %.1f %.1f\n", Top[k].Poly,
                   Top[k].Tri, Top[k].I, Top[k].J, Steps, Top[k].Dn, Top[k].W.X, Top[k].W.Y, Top[k].W.Z);
            // The triangle's corners: where they were, how the pieces moved them, and the field's move there.
            if (k < 8)
            {
                const int t = R.PolyFirst[Top[k].Poly] + Top[k].Tri;
                const FV3 Np = RdxSphere::V3(Polys[Top[k].Poly].Nx, Polys[Top[k].Poly].Ny, Polys[Top[k].Poly].Nz);
                for (int c = 0; c < 3; c++)
                {
                    const int v = R.Tri[t * 3 + c];
                    FV3       Mv = RdxSphere::V3(0.0, 0.0, 0.0);
                    const bool bOn = RdxLevelRound::MoveOnPolygon(R, Top[k].Poly, R.Orig[v], 2.0, Mv);
                    const FV3  Pc  = R.Moved[v] - R.Orig[v];
                    printf("    corner %d (place %d): %.2f %.2f %.2f, the pieces' move %.3f %.3f %.3f (%.3f along the normal), "
                           "the field's %s %.3f %.3f %.3f (%.3f)\n", c, v, R.Orig[v].X, R.Orig[v].Y, R.Orig[v].Z, Pc.X, Pc.Y,
                           Pc.Z, RdxSphere::Dot(Pc, Np), bOn ? "" : "(no face)", Mv.X, Mv.Y, Mv.Z, RdxSphere::Dot(Mv, Np));
                }
            }
        }
    }
    {
        auto Mb = [](double Bytes) { return Bytes / 1048576.0; };
        const double Points = Mb((double)R.Orig.Room * sizeof(FV3) + (double)R.Moved.Room * sizeof(FV3));
        const double TriMb = Mb((double)R.Tri.Room * sizeof(int)), Turns = Mb((double)R.TriTurn.Room * sizeof(float));
        const double Faces = Mb((double)R.FaceKind.Room + (double)R.FaceCorner.Room * sizeof(FV3) +
                                (double)R.FaceTerm.Room * sizeof(float));
        const double Light = Mb((double)R.LightSides.Room * sizeof(RdxSphere::FLightSide) +
                                (double)R.LightParts.Room * sizeof(RdxSphere::FLightPart));
        const double Blocks = Mb((double)(R.Tri.Num / 3) * 21.0 * sizeof(float));
        printf("memory, MB: places %.1f (%d of them), triangles %.1f, turns %.1f, faces %.1f, light sides %.1f; the pieces' "
               "blocks once all are drawn %.1f; the shadow world's two lists %.1f\n", Points, R.Orig.Num, TriMb, Turns, Faces,
               Light, Blocks, Mb((double)(R.Tri.Num / 3) * 72.0));
    }
    printf("parts, ms: welding %.1f, T-junctions %.1f, triangles %.1f, shape %.1f, pieces %.1f, made again %.1f, moves %.1f, "
           "polygons %.1f, outlines %.1f; places taken out %d, stuck %d, pieces %d, quads %d\n", R.PartMs[0], R.PartMs[1],
           R.PartMs[2], R.PartMs[3], R.PartMs[4], R.PartMs[5], R.PartMs[6], R.PartMs[7], R.PartMs[8], R.Through, R.Stuck,
           R.Pieces, R.Quads);
    printf("polygons %d, drawn and staying %d, triangles %d (soft edges %d, creases %d, T-junctions %d, left %d); moved %d "
           "into %d pieces, the most %.3f of %.0f; over the cap %d, openings %d (the most %.3f), turned over %d (in polygons "
           "staying in their planes %d, slivers %d, showing %d), flat %d, bad light sides %d of %d, the card's copy missed "
           "%d, off by %.4f; %.0f ms\n",
           NumPolys, R.Walls, R.Segments, R.SoftEdges, R.Creases, R.TPoints, R.Folded, R.Touched, Tris, R.LargestMove, Cap,
           Over, Open, OpenMost, Turned, TurnedInPlane, TurnedSliver, TurnedShowing, Flat, BadSides, R.LightSides.Num,
           CardMiss, CardOff, Ms);
    printf("the card's copy off the CPU: past a tenth %d (another polygon's %d), past a unit %d (another polygon's %d)\n",
           CardOffTenth, CardOtherTenth, CardOffUnit, CardOtherUnit);
    if (WorstOwn >= 0)
        printf("the worst: at %.2f %.2f %.2f, its polygon %d (normal %.3f %.3f %.3f), the card's %d (normal %.3f %.3f %.3f); "
               "moves %.3f %.3f %.3f and %.3f %.3f %.3f\n", WorstAt.X, WorstAt.Y, WorstAt.Z, WorstOwn, Polys[WorstOwn].Nx,
               Polys[WorstOwn].Ny, Polys[WorstOwn].Nz, WorstFound,
               (WorstFound >= 0 && WorstFound < NumPolys) ? Polys[WorstFound].Nx : 0.0,
               (WorstFound >= 0 && WorstFound < NumPolys) ? Polys[WorstFound].Ny : 0.0,
               (WorstFound >= 0 && WorstFound < NumPolys) ? Polys[WorstFound].Nz : 0.0, WorstCpu.X, WorstCpu.Y, WorstCpu.Z,
               WorstGpu.X, WorstGpu.Y, WorstGpu.Z);
    // LRC_COMPACT=1, last of all: the places kept in single precision, as the renderer keeps them once the level is made
    // (FResult::Compact) - the card's copy made of them set against the one made before, word by word, and the moves of
    // the points laid on the polygons (HoldOnPolygon, the decals' way) at every moved polygon's places.
    if (getenv("LRC_COMPACT"))
    {
        TList<unsigned int> Before, After;
        const bool          bBefore = R.Touched > 0 && RdxLevelRound::PackRound(R, Before);
        std::vector<FV3>    HeldBefore;
        std::vector<int>    HeldPoly, HeldPlace;
        for (int p = 0; p < NumPolys && p < R.PolyTris.Num; p++)
            for (int t = 0; t < R.PolyTris[p]; t++)
                for (int k = 0; k < 3; k++)
                {
                    const int v = R.Tri[(R.PolyFirst[p] + t) * 3 + k];
                    FV3       M = R.Moved[v] - R.Orig[v];
                    RdxLevelRound::HoldOnPolygon(R, p, R.Orig[v], M);
                    HeldBefore.push_back(M);
                    HeldPoly.push_back(p);
                    HeldPlace.push_back(v);
                }
        const size_t Was = R.Bytes();
        const bool   bKept = R.Compact();
        const bool   bAfter = R.Touched > 0 && RdxLevelRound::PackRound(R, After);
        int          WordsOff = 0;
        if (bBefore && bAfter && Before.Num == After.Num)
            for (int i = 0; i < Before.Num; i++)
                WordsOff += (Before[i] != After[i]) ? 1 : 0;
        double HeldMost = 0.0;
        for (size_t i = 0; i < HeldBefore.size(); i++)
        {
            const FV3 O = R.OrigAt(HeldPlace[i]);
            FV3       M = R.MovedAt(HeldPlace[i]) - O;
            RdxLevelRound::HoldOnPolygon(R, HeldPoly[i], O, M);
            const double D = RdxSphere::Length(M - HeldBefore[i]);
            HeldMost       = (D > HeldMost) ? D : HeldMost;
        }
        printf("compact: %s; the result %.1f -> %.1f MB; the card's copy %d words, %d of them otherwise (%s); held moves "
               "at %d places, off by %.6f at the most\n",
               bKept ? "kept in single precision" : "NO MEMORY", Was / 1048576.0, R.Bytes() / 1048576.0, After.Num,
               WordsOff, (bBefore && bAfter && Before.Num == After.Num) ? "as long" : "NOT AS LONG",
               (int)HeldBefore.size(), HeldMost);
    }
    return (Over || Open || TurnedShowing || BadSides) ? 1 : 0;
}

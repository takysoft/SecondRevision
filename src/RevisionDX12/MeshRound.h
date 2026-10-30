// SecondRevision - the models rounded (SpherifierDistance=<units>, SpherifierAngle=<degrees>; RenderDevice's
// DrawGouraudPolygon, SolidPass.hlsl's DSMesh): a model's triangles bowed out as a smooth surface through their corners
// would be, with its hard edges kept.
//
// The engine gives each vertex of a model one normal, averaged over every face round it: a box's corner, a square pot's
// edge, a can's rim where the lid meets the side all come in as if the shape were round there, and a model of flat parts
// bowed by them comes out blown up. So the model's own faces decide it:
// - The places are welded first: two vertices nearer than a hair of the model's size are one.
// - Faces that lie in one plane and share an edge are one flat polygon (a quad cut in two, a lid cut into a fan); the
//   edges between its triangles are inner edges. Every other edge is a true edge. Two polygons that turn by at most the
//   shape's turn at a true edge (Make's MostTurn: 180 degrees less the SpherifierAngle) are one smooth surface there;
//   where they turn more, the edge is a crease. The faces round a vertex fall into fans, cut apart by the creases, and a
//   corner's normal is its fan's: the fan's face normals summed, each weighed by its face's angle at the vertex, in the
//   shape as it stands in the draw.
// - Each edge is a curve between its two corners, which stay where they are: at each end it leaves the corner along a
//   way of the surface there, turned off the straight edge by a term of that end's - the same from both of its faces, so
//   nothing opens between them (the patch adds the edges' curves up, each weighed by its two corners' shares, and a
//   little of all of them for its middle: PatchMoveAt). Where its two faces meet in one fan, the edge leaves along the
//   way between it and the true edge that carries it on past the corner, laid into the fan's plane - a ring of edges
//   round a can follows its circle - or along its own way laid into that plane where none carries it on; where a crease
//   parts them, along the crease's own line there (the two fans' normals crossed): a can's rim bows round in the lid's
//   plane, a box's edge stays straight.
// - A bend stays near its corner where the surface goes on round it in short pieces: along an edge longer than the true
//   edge that carries the curve on past the corner (the most nearly straight on), the bend is done within that one's
//   length - a bottle's long side stays straight up to its short shoulder, which rounds. And within the length that
//   bends it out by no more than the spherifier's number, the most anything moves: a long wall beside a soft corner
//   curves near the corner, not all along. An inner edge bends as the corner's fan tilts along each of the true edges
//   that bound its polygon there, each tilt within the room it would take on that edge - the polygon's middle goes along
//   with its edges.
// - An open edge, one face's only, leaves each end along the outline's own way there (as a soft edge goes on into the
//   next of its chain), where the outline turns softly - a hexagonal opening rounds into a circle, and a hexagonal cap
//   laid in it the same way - and stays straight where it turns sharply or goes on straight: a piece laid along another
//   keeps to it.
// - An edge with more than two faces, two faces turned opposite ways, or one beside a face that is not drawn, does not
//   bow: both sides of it stay where the engine put them.
// - No end of an edge bends further than half the most a soft edge turns by (EndSine): a fan that wraps round a small
//   part would bend an edge by as much as its length - a spike out of the model. At a crease, an end whose edge strays
//   further than that from the crease's own line follows no line at all: it stays straight.
//
// The model (FModel) holds what no pose changes - its faces, the faces round each vertex, and its faces by how far apart
// their vertices are numbered, by which a triangle the engine draws is known (its samples lie as far apart, a vertex a
// sample). The shape (FShape) holds the welds, polygons, fans and kinds of edge, made once from one pose; the places of a
// draw are read where they lie (FPose: the engine's own samples, a stride apart), never copied. A model that does not
// move in itself - a chair, a crate, a pot - is bent once (MakeRest): a later draw that is the same pose turned, scaled
// and moved (Fit, held against every vertex) takes those terms turned and scaled, with no fans worked out again.
//
// Engine-independent, as RenderMath.h and Spherify.h: the unit tests run the same code.
#pragma once

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "Spherify.h"

namespace RdxMeshRound
{

using RdxSphere::MaxOf;
using RdxSphere::MinOf;
using RdxSphere::TList;

struct FV
{
    float X, Y, Z;
};
inline FV V(float X, float Y, float Z)
{
    FV R = { X, Y, Z };
    return R;
}
inline FV operator+(FV A, FV B) { return V(A.X + B.X, A.Y + B.Y, A.Z + B.Z); }
inline FV operator-(FV A, FV B) { return V(A.X - B.X, A.Y - B.Y, A.Z - B.Z); }
inline FV operator*(FV A, float S) { return V(A.X * S, A.Y * S, A.Z * S); }
inline float Dot(FV A, FV B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
inline FV Cross(FV A, FV B) { return V(A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X); }
inline float Length(FV A) { return sqrtf(Dot(A, A)); }
inline bool Same(FV A, FV B) { return A.X == B.X && A.Y == B.Y && A.Z == B.Z; }
inline FV Unit(FV A)
{
    const float L = Length(A);
    return (L > 1e-12f) ? A * (1.0f / L) : V(0.0f, 0.0f, 0.0f);
}

// The cosine of the most two faces of one smooth surface turn by at their edge, Turn degrees.
inline float SoftCosOf(float Turn) { return (float)cos(Turn * RdxSphere::Pi / 180.0); }
// The sine of the most an end of an edge bends by: half the most a soft edge turns by, as a regular polygon's sides do.
inline float EndSinOf(float Turn) { return (float)sin(0.5 * Turn * RdxSphere::Pi / 180.0); }

// Two faces whose normals differ by less than this lie in one plane: one polygon.
static const float FlatCos = 0.99996f;    // half a degree
// A bend's room never shrinks below this share of its edge.
static const float TauLeast = 0.02f;
// The most a bend moves its edge, as a share of its term: the curve s (1 - s)^2 tops out at 4/27, a third along.
static const float BendTop = 4.0f / 27.0f;

// The shape of a bend at one end of an edge: s (1 - s)^2 for s in [0, 1] - s how far along its room - none past it. Its
// slope is one where it starts, so the edge leaves its corner along the surface there, and none where it ends.
inline float BendOf(float S) { return (S < 1.0f) ? S * (1.0f - S) * (1.0f - S) : 0.0f; }

// An edge's terms, from its start to its end: at each end two bends (an inner edge's from the two true edges that bound
// its polygon there, a true edge's own and none), each with the share of the edge it is done within, its room.
enum { EDGE_SLOTS = 4 };   // the start's two, then the end's two
struct FEdgeTerms
{
    FV    T[EDGE_SLOTS];
    float Room[EDGE_SLOTS];
};
// The curve of an edge so given, at U from its start (SolidPass.hlsl's MeshEdge): each bend scaled by its room.
inline FV EdgeCurveAt(const FEdgeTerms& E, float U)
{
    return E.T[0] * (E.Room[0] * BendOf(U / E.Room[0])) + E.T[1] * (E.Room[1] * BendOf(U / E.Room[1])) +
           E.T[2] * (E.Room[2] * BendOf((1.0f - U) / E.Room[2])) + E.T[3] * (E.Room[3] * BendOf((1.0f - U) / E.Room[3]));
}

// A patch's move off its flat triangle at the corners' shares B0, B1, B2, before the cap (SolidPass.hlsl's DSMesh; E: the
// edges from corner k to k + 1): each edge's curve weighed by the cube of its two corners' shares together - only an
// edge's own curve lies on it - and half of every bend, at its room, for the middle, as a cubic triangle's centre is
// placed by its edges.
inline FV PatchMoveAt(const FEdgeTerms* E, float B0, float B1, float B2)
{
    const float B[3] = { B0, B1, B2 };
    FV          Move = V(0.0f, 0.0f, 0.0f), Middle = V(0.0f, 0.0f, 0.0f);
    for (int e = 0; e < 3; e++)
    {
        const float Ba = B[e], Bb = B[(e + 1) % 3], S = Ba + Bb;
        if (S > 1e-6f)
            Move = Move + EdgeCurveAt(E[e], Bb / S) * (S * S * S);
        for (int k = 0; k < EDGE_SLOTS; k++)
            Middle = Middle + E[e].T[k] * E[e].Room[k];
    }
    return Move + Middle * (0.5f * B0 * B1 * B2);
}

// The most outline edges a flat polygon's one move over it reads (PolyPlaneMoveAt); a polygon of more keeps its faces'
// own patches.
enum { POLY_EDGES_MOST = 32 };

// The share along its plane of a point's move in a flat polygon of several faces (SolidPass.hlsl's MeshPolyPlaneMove):
// each outline edge's curve where the point lies across from it - past an end, the curve turned back on itself there,
// so it goes on smoothly - along the plane (Normal), weighed by one over the square of how far the point lies from the
// edge. One smooth move over the whole polygon, on its outline the outline's own: a picture painted on it keeps its
// lines smooth, where its faces' patches would each drag it their own way. Terms: the outline edges' terms, each from
// its end A to its end B; Ends: those ends, A then B, two to an edge.
inline FV PolyPlaneMoveAt(const FEdgeTerms* Terms, const FV* Ends, int Num, FV Normal, FV Flat)
{
    FV    Sum    = V(0.0f, 0.0f, 0.0f);
    float Weight = 0.0f;
    for (int g = 0; g < Num; g++)
    {
        const FV    A = Ends[g * 2], Way = Ends[g * 2 + 1] - A;
        const float L2 = Dot(Way, Way);
        const float T  = Dot(Flat - A, Way) / ((L2 > 1e-12f) ? L2 : 1e-12f);
        const FV    Off = Flat - (A + Way * ((T < 0.0f) ? 0.0f : (T > 1.0f) ? 1.0f : T));
        const float D2  = Dot(Off, Off);
        const float Past = (T < 0.0f) ? -T : (T > 1.0f) ? 2.0f - T : T;
        const float U    = (Past < 0.0f) ? 0.0f : (Past > 1.0f) ? 1.0f : Past;
        FV          C    = EdgeCurveAt(Terms[g], U) * ((T < 0.0f || T > 1.0f) ? -1.0f : 1.0f);
        C                = C - Normal * Dot(C, Normal);
        if (D2 < 1e-8f)
            return C;
        Sum    = Sum + C * (1.0f / D2);
        Weight = Weight + 1.0f / D2;
    }
    return (Weight > 0.0f) ? Sum * (1.0f / Weight) : V(0.0f, 0.0f, 0.0f);
}

inline unsigned int HashOf(unsigned int A, unsigned int B, unsigned int C)
{
    unsigned int H = A * 0x9E3779B1u;
    H ^= B + 0x7F4A7C15u + (H << 6) + (H >> 2);
    H ^= C + 0x165667B1u + (H << 6) + (H >> 2);
    H ^= H >> 15;
    H *= 0x2C1B3C6Du;
    H ^= H >> 12;
    return H;
}

// The angle between two directions, 0 .. pi, from its sine and cosine times one length (Y >= 0) - to within 1e-5.
inline float AngleOf(float Y, float X)
{
    const float AX = fabsf(X);
    const float Hi = (AX > Y) ? AX : Y;
    if (!(Hi > 0.0f))
        return 0.0f;
    const float A = ((AX > Y) ? Y : AX) / Hi;
    const float S = A * A;
    float       R = A * (0.99997726f +
                         S * (-0.33262347f + S * (0.19354346f + S * (-0.11643287f + S * (0.05265332f - S * 0.01172120f)))));
    if (Y > AX)
        R = 1.57079633f - R;
    if (X < 0.0f)
        R = 3.14159265f - R;
    return R;
}

inline int TableSizeFor(int Count)
{
    int Size = 16;
    while (Size < Count * 2)
        Size *= 2;
    return Size;
}

inline void Sorted(int A, int B, int C, int* S)
{
    if (A > B) { const int T = A; A = B; B = T; }
    if (B > C) { const int T = B; B = C; C = T; }
    if (A > B) { const int T = A; A = B; B = T; }
    S[0] = A;
    S[1] = B;
    S[2] = C;
}

// The distance from point Q to the triangle A, B, C (the closest point found region by region).
inline float DistanceToTriangle(FV Q, FV A, FV B, FV C)
{
    const FV    AB = B - A, AC = C - A, AQ = Q - A;
    const float D1 = Dot(AB, AQ), D2 = Dot(AC, AQ);
    if (D1 <= 0.0f && D2 <= 0.0f)
        return Length(AQ);
    const FV    BQ = Q - B;
    const float D3 = Dot(AB, BQ), D4 = Dot(AC, BQ);
    if (D3 >= 0.0f && D4 <= D3)
        return Length(BQ);
    const float VC = D1 * D4 - D3 * D2;
    if (VC <= 0.0f && D1 >= 0.0f && D3 <= 0.0f)
        return Length(Q - (A + AB * (D1 / (D1 - D3))));
    const FV    CQ = Q - C;
    const float D5 = Dot(AB, CQ), D6 = Dot(AC, CQ);
    if (D6 >= 0.0f && D5 <= D6)
        return Length(CQ);
    const float VB = D5 * D2 - D1 * D6;
    if (VB <= 0.0f && D2 >= 0.0f && D6 <= 0.0f)
        return Length(Q - (A + AC * (D2 / (D2 - D6))));
    const float VA = D3 * D6 - D5 * D4;
    if (VA <= 0.0f && (D4 - D3) >= 0.0f && (D5 - D6) >= 0.0f)
        return Length(Q - (B + (C - B) * ((D4 - D3) / ((D4 - D3) + (D5 - D6)))));
    const float Den = VA + VB + VC;
    if (!(fabsf(Den) > 1e-20f))
        return Length(AQ);
    return Length(Q - (A + AB * (VB / Den) + AC * (VC / Den)));
}

// A pose: a vertex's place, Stride bytes after the one before it from Base.
struct FPose
{
    const char* Base   = nullptr;
    int         Stride = 0;
    FV At(int v) const { return *(const FV*)(Base + (size_t)v * (size_t)Stride); }
};

// What of a model no pose changes.
struct FModel
{
    int        Verts = 0;
    int        Faces = 0;
    TList<int> Corner;      // 3 a face: its vertices, as the model lists them
    TList<int> VertStart;   // Verts + 1: where each vertex's faces start in VertFaces
    TList<int> VertFaces;
    TList<int> Table;       // the faces by their three vertices (FindFace); -1 empty
    int        TableMask = 0;
    // The faces by how far apart their vertices are numbered (Signature): the distinct pairs, and their faces.
    TList<int> SigKey;      // a slot: its pair, or -1 -1
    TList<int> SigStart;    // a slot: where its faces start in SigFaces,
    TList<int> SigCount;    // and how many
    TList<int> SigFaces;
    int        SigMask = 0;

    // Corners: 3 a face, the model's vertex numbers. False: a vertex out of range - nothing of the model is rounded.
    bool Make(const int* Corners, int FaceCount, int VertCount)
    {
        Verts = Faces = 0;
        if (!Corners || FaceCount <= 0 || VertCount <= 0)
            return false;
        for (int i = 0; i < FaceCount * 3; i++)
            if (Corners[i] < 0 || Corners[i] >= VertCount)
                return false;
        if (!Corner.Resize(FaceCount * 3) || !VertStart.Resize(VertCount + 1) || !VertFaces.Resize(FaceCount * 3))
            return false;
        memcpy(Corner.Data, Corners, sizeof(int) * (size_t)FaceCount * 3);
        Verts = VertCount;
        Faces = FaceCount;
        for (int v = 0; v <= VertCount; v++)
            VertStart[v] = 0;
        for (int c = 0; c < FaceCount * 3; c++)
            VertStart[Corner[c] + 1]++;
        for (int v = 0; v < VertCount; v++)
            VertStart[v + 1] += VertStart[v];
        {
            TList<int> Fill;
            if (!Fill.Resize(VertCount))
                return false;
            for (int v = 0; v < VertCount; v++)
                Fill[v] = VertStart[v];
            for (int c = 0; c < FaceCount * 3; c++)
                VertFaces[Fill[Corner[c]]++] = c / 3;
        }
        // The faces by their vertices, the three in rising order.
        {
            const int Size = TableSizeFor(FaceCount);
            if (!Table.Resize(Size))
                return false;
            TableMask = Size - 1;
            for (int i = 0; i < Size; i++)
                Table[i] = -1;
            for (int f = 0; f < FaceCount; f++)
            {
                int S[3];
                Sorted(Corner[f * 3], Corner[f * 3 + 1], Corner[f * 3 + 2], S);
                unsigned int H = HashOf((unsigned int)S[0], (unsigned int)S[1], (unsigned int)S[2]) &
                                 (unsigned int)TableMask;
                bool Have = false;
                while (Table[(int)H] >= 0)
                {
                    int       T[3];
                    const int g = Table[(int)H];
                    Sorted(Corner[g * 3], Corner[g * 3 + 1], Corner[g * 3 + 2], T);
                    if (T[0] == S[0] && T[1] == S[1] && T[2] == S[2])
                    {
                        Have = true;
                        break;
                    }
                    H = (H + 1) & (unsigned int)TableMask;
                }
                if (!Have)
                    Table[(int)H] = f;
            }
        }
        // The faces by how far apart their vertices are numbered.
        {
            const int Size = TableSizeFor(FaceCount);
            if (!SigKey.Resize(Size * 2) || !SigStart.Resize(Size) || !SigCount.Resize(Size) ||
                !SigFaces.Resize(FaceCount))
                return false;
            SigMask = Size - 1;
            for (int i = 0; i < Size; i++)
            {
                SigKey[i * 2] = SigKey[i * 2 + 1] = -1;
                SigCount[i]                       = 0;
            }
            TList<int> SlotOf;
            if (!SlotOf.Resize(FaceCount))
                return false;
            for (int f = 0; f < FaceCount; f++)
            {
                int S[3];
                Sorted(Corner[f * 3], Corner[f * 3 + 1], Corner[f * 3 + 2], S);
                SlotOf[f] = SigSlot(S[1] - S[0], S[2] - S[0], true);
                SigCount[SlotOf[f]]++;
            }
            int Next = 0;
            for (int i = 0; i < Size; i++)
            {
                SigStart[i] = Next;
                Next += SigCount[i];
                SigCount[i] = 0;
            }
            for (int f = 0; f < FaceCount; f++)
                SigFaces[SigStart[SlotOf[f]] + SigCount[SlotOf[f]]++] = f;
        }
        return true;
    }

    int SigSlot(int D1, int D2, bool bAdd)
    {
        unsigned int H = HashOf((unsigned int)D1, (unsigned int)D2, 0x5167u) & (unsigned int)SigMask;
        while (SigKey[(int)H * 2] >= 0)
        {
            if (SigKey[(int)H * 2] == D1 && SigKey[(int)H * 2 + 1] == D2)
                return (int)H;
            H = (H + 1) & (unsigned int)SigMask;
        }
        if (!bAdd)
            return -1;
        SigKey[(int)H * 2]     = D1;
        SigKey[(int)H * 2 + 1] = D2;
        return (int)H;
    }

    // The faces whose vertices, in rising order, are numbered D1 and D2 after the first: how many, from *First on.
    int Signature(int D1, int D2, const int** First)
    {
        if (Faces <= 0 || D1 < 0 || D2 < D1)
            return 0;
        const int Slot = SigSlot(D1, D2, false);
        if (Slot < 0)
            return 0;
        *First = &SigFaces[SigStart[Slot]];
        return SigCount[Slot];
    }

    // The face of these three vertices, in any order; -1 none.
    int FindFace(int A, int B, int C) const
    {
        if (Faces <= 0)
            return -1;
        int S[3];
        Sorted(A, B, C, S);
        unsigned int H = HashOf((unsigned int)S[0], (unsigned int)S[1], (unsigned int)S[2]) & (unsigned int)TableMask;
        while (Table[(int)H] >= 0)
        {
            const int g = Table[(int)H];
            int       T[3];
            Sorted(Corner[g * 3], Corner[g * 3 + 1], Corner[g * 3 + 2], T);
            if (T[0] == S[0] && T[1] == S[1] && T[2] == S[2])
                return g;
            H = (H + 1) & (unsigned int)TableMask;
        }
        return -1;
    }

    // Vertex v's normal over every face at it, each as long as twice its area, in pose P - as the engine makes the
    // normals it lights a model by. Zero where its faces have no area.
    FV PlainNormal(const FPose& P, int v) const
    {
        FV Sum = V(0.0f, 0.0f, 0.0f);
        for (int i = VertStart[v]; i < VertStart[v + 1]; i++)
        {
            const int f = VertFaces[i];
            const FV  A = P.At(Corner[f * 3]);
            Sum         = Sum + Cross(P.At(Corner[f * 3 + 1]) - A, P.At(Corner[f * 3 + 2]) - A);
        }
        const float L = Length(Sum);
        return (L > 1e-12f) ? Sum * (1.0f / L) : V(0.0f, 0.0f, 0.0f);
    }
};

// A model's faces as one shape: welds, polygons, fans and the kinds of its edges, made once a model from one pose (Make);
// the fans' normals and the edges' terms from the pose of each draw (Pose, EdgeTerms).
struct FShape
{
    // The kinds of edge.
    enum
    {
        Edge_Fixed = 0,   // does not bow: more than two faces, two turned opposite ways, beside a face not drawn, or an
                          // open edge where the open edges stay (Make's bOpenFree)
        Edge_Inner,       // between two triangles of one flat polygon
        Edge_Soft,        // between two polygons of one smooth surface
        Edge_Crease,      // between two polygons that turn more
        Edge_Open         // one face's only, free: it follows its outline's way where the outline turns softly
    };

    const FModel* M          = nullptr;
    float         SoftCosine = 1.0f;     // the most two faces of one surface turn by (Make's MostTurn): its cosine,
    float         EndSine    = 0.0f;     // EndSinOf it,
    float         FullSine   = 0.0f;     // and its own sine: the most an end turns where a chain of edges turns there
    float         Cap        = 8.0f;     // the most anything moves, in the pose's units (Make's MostMove)
    bool          bRingPasses = false;   // an edge of a closed ring passes what stands out beside it (Make's)
    int           Fans       = 0;
    int           Edges      = 0;
    TList<int>    Weld;        // a vertex: the first vertex at its place
    TList<int>    Fan;         // 3 a face: the fan each corner is in
    TList<int>    EdgeOf;      // 3 a face: its edge from corner k to k + 1
    TList<int>    Poly;        // a face: its flat polygon (the lowest of its faces)
    TList<int>    FanStart;    // Fans + 1: where each fan's faces start in FanFaces
    TList<int>    FanFaces;
    TList<int>    FanVertex;   // a fan: its welded vertex
    // An edge: its welded ends (EdgeA < EdgeB), its kind, its first and second face corner (the corner it starts at in
    // that face; -1 none), and at each end (2 an edge: A, then B): the true edge that carries the curve on past it - the
    // most nearly straight on, turning by at most a soft turn (-1 none) -, the shortest other true edge there (-1 none),
    // and for an inner edge, the two true edges that bound its polygon there (-1 none).
    TList<int>           EdgeA, EdgeB, EdgeC0, EdgeC1, EdgeCount;
    TList<unsigned char> EdgeKind;
    // Each edge a sheet's face goes along with (Make's Sheet), three faces: the sheet's, then the two in one plane.
    TList<int>           SheetAlong;
    TList<int>           OnEdge, NearEdge, Bound0, Bound1;
    // A true edge on a closed ring: from it, each edge that carries the curve on past the one before (OnEdge) leads back
    // round to it - a table's rim, a pillar's ring.
    TList<unsigned char> OnRing;
    // A true edge: the most each of its ends' bends may stand off it (EdgeCapOf) - a quarter of how deep the flat
    // polygon on either side of it reaches across, the shallower - so that the two sides of a thin part never bow past
    // each other; none past Cap.
    TList<float> EdgeCap;
    TList<int>   PolyEdgeStart, PolyEdgeList;   // a flat polygon (its lowest face): where its outline edges start, and them
    // An open edge's neighbours along the outline, at its end A and at its end B (-1: none, or more than one).
    TList<int> OutlineA, OutlineB;
    // The pose of the draw; the fans' normals, the faces' normals and angles, and the edges' terms in it as they are
    // asked for.
    FPose        P;
    int          Stamp = 0;
    TList<FV>    FanNormalOf;
    TList<int>   FanStampOf;
    TList<FV>    FaceNormalOf;
    TList<float> FaceAngleOf;
    TList<int>   FaceStampOf;
    // An edge's terms, EDGE_SLOTS an edge: its two bends at end A, then its two at end B, and their rooms.
    TList<FV>    Term;
    TList<float> Room;
    TList<int>   TermStampOf;
    // The rest (MakeRest): every edge's terms in one pose, and every welded vertex's place in it, held against the next
    // poses; RestA, RestB, RestC of them the three a pose's turn is taken from.
    TList<FV>    RestTerm;
    TList<float> RestRoom;
    TList<int>   RestRef;
    TList<FV>    RestAt;
    bool         bRest = false;
    int          RestA = 0, RestB = 0, RestC = 0;
    float        RestSize = 0.0f;
    // The draw's fit (Fit): its pose is the rest's turned, scaled (Turn, by rows) and moved.
    bool  bFit = false;
    float Turn[9];

    // Round: a face is drawn (null: all are); Pose: the vertices' places the shape is made from; MostTurn: the most two
    // faces of one smooth surface turn by at their edge, in degrees; MostMove: the most anything moves, in the pose's
    // units. WeldHair: how near two places are one (none: a ten-thousandth of the model's reach); bOpenFree: whether an
    // open edge may follow its outline (the level's are where its drawn surfaces end - at a mover, a portal - and stay).
    // FlatHair: how near each of two faces' far corners lies to the other's plane for them to lie in one plane where they
    // turn by more than FlatCos - none for the level, whose places are its own; for a model (below zero) a two-thousandth
    // of its reach: the faces of a small part laid in one plane come out of a model turned off each other by a degree or
    // two (a microscope's lens ring), by how its places were made and kept, and nearer than that is one plane.
    // Sheet: a face of a sheet drawn from both sides (null: none) - lying across a flat polygon's inside, on an edge of
    // that polygon's, it goes along with the polygon there, rather than the three faces holding the edge.
    // bRingPass: whether an edge of a closed ring (OnRing) passes what stands out of the front of a flat polygon beside
    // it, where the edge's other polygon lies behind that one - a table's rim over the leg under it - rather than being
    // held by how deep the polygon reaches there (EdgeCapOf): the two cannot meet, and the polygon between them only
    // folds along its own plane, as the level draws it (the level's; a model's edges stay held).
    bool Make(const FModel& Model, const unsigned char* Round, const FPose& Pose, float MostTurn, float MostMove,
              float WeldHair = -1.0f, bool bOpenFree = true, float FlatHair = -1.0f,
              const unsigned char* Sheet = nullptr, bool bRingPass = false)
    {
        M          = &Model;
        Fans       = 0;
        Edges      = 0;
        SheetAlong.Clear();
        SoftCosine = SoftCosOf(MostTurn);
        EndSine    = EndSinOf(MostTurn);
        FullSine   = sqrtf(MaxOf(1.0f - SoftCosine * SoftCosine, 0.0f));
        Cap        = MostMove;
        bRingPasses = bRingPass;
        bRest = bFit = false;
        const int FaceCount = Model.Faces, VertCount = Model.Verts;
        if (FaceCount <= 0 || VertCount <= 0 || !Pose.Base)
            return false;
        if (!Weld.Resize(VertCount) || !Fan.Resize(FaceCount * 3) || !EdgeOf.Resize(FaceCount * 3) ||
            !Poly.Resize(FaceCount))
            return false;
        const int* const Corner = Model.Corner.Data;
        P                       = Pose;

        // The model's reach in the pose: the hair the places are welded within, and how near another face an open edge
        // lies against it.
        FV Lo = Pose.At(0), Hi = Lo;
        for (int v = 1; v < VertCount; v++)
        {
            const FV Q = Pose.At(v);
            Lo         = V(MinOf(Lo.X, Q.X), MinOf(Lo.Y, Q.Y), MinOf(Lo.Z, Q.Z));
            Hi         = V(MaxOf(Hi.X, Q.X), MaxOf(Hi.Y, Q.Y), MaxOf(Hi.Z, Q.Z));
        }
        const float Reach    = Length(Hi - Lo);
        const float FlatNear = (FlatHair >= 0.0f) ? FlatHair : Reach / 2048.0f;

        // The places welded: within a hair of the model's size, by a grid of cells that size.
        {
            const float Hair = (WeldHair > 0.0f) ? WeldHair : MaxOf(1e-4f * Reach, 1e-6f);
            const int   Size = TableSizeFor(VertCount);
            TList<int>  Head, Next;
            if (!Head.Resize(Size) || !Next.Resize(VertCount))
                return false;
            for (int i = 0; i < Size; i++)
                Head[i] = -1;
            auto CellOf = [&](float X) { return (int)floorf(X / Hair); };
            for (int v = 0; v < VertCount; v++)
            {
                const FV  Q  = Pose.At(v);
                const int CX = CellOf(Q.X), CY = CellOf(Q.Y), CZ = CellOf(Q.Z);
                Weld[v]      = v;
                for (int dz = -1; dz <= 1 && Weld[v] == v; dz++)
                    for (int dy = -1; dy <= 1 && Weld[v] == v; dy++)
                        for (int dx = -1; dx <= 1 && Weld[v] == v; dx++)
                        {
                            const unsigned int H = HashOf((unsigned int)(CX + dx), (unsigned int)(CY + dy),
                                                          (unsigned int)(CZ + dz)) & (unsigned int)(Size - 1);
                            for (int w = Head[(int)H]; w >= 0; w = Next[w])
                            {
                                const FV W = Pose.At(w);
                                if (CellOf(W.X) == CX + dx && CellOf(W.Y) == CY + dy && CellOf(W.Z) == CZ + dz &&
                                    fabsf(W.X - Q.X) <= Hair && fabsf(W.Y - Q.Y) <= Hair && fabsf(W.Z - Q.Z) <= Hair)
                                {
                                    Weld[v] = w;
                                    break;
                                }
                            }
                        }
                if (Weld[v] == v)
                {
                    const unsigned int H = HashOf((unsigned int)CX, (unsigned int)CY, (unsigned int)CZ) &
                                           (unsigned int)(Size - 1);
                    Next[v]      = Head[(int)H];
                    Head[(int)H] = v;
                }
            }
        }

        // Each face's normal in the pose, unit - zero for a face of no area, which no edge joins to another.
        TList<FV> FaceN;
        if (!FaceN.Resize(FaceCount))
            return false;
        for (int f = 0; f < FaceCount; f++)
        {
            const FV A = Pose.At(Weld[Corner[f * 3]]);
            FaceN[f]   = Unit(Cross(Pose.At(Weld[Corner[f * 3 + 1]]) - A, Pose.At(Weld[Corner[f * 3 + 2]]) - A));
        }
        auto Drawn = [&](int f) { return !Round || Round[f]; };

        // The edges: by their two welded ends, the face corners that have each and how many - and of an edge of
        // three faces, the third corner (Third).
        const int  EdgeSize = TableSizeFor(FaceCount * 3);
        TList<int> EdgeSlot, Third;
        if (!EdgeSlot.Resize(EdgeSize) || !EdgeA.Resize(FaceCount * 3) || !EdgeB.Resize(FaceCount * 3) ||
            !EdgeC0.Resize(FaceCount * 3) || !EdgeC1.Resize(FaceCount * 3) || !EdgeCount.Resize(FaceCount * 3) ||
            !EdgeKind.Resize(FaceCount * 3) || !Third.Resize(FaceCount * 3))
            return false;
        for (int i = 0; i < EdgeSize; i++)
            EdgeSlot[i] = -1;
        for (int c = 0; c < FaceCount * 3; c++)
        {
            const int    A  = Weld[Corner[c]];
            const int    B  = Weld[Corner[(c / 3) * 3 + (c % 3 + 1) % 3]];
            const int    Lw = (A < B) ? A : B;
            const int    Hg = (A < B) ? B : A;
            unsigned int H  = HashOf((unsigned int)Lw, (unsigned int)Hg, 0x51u) & (unsigned int)(EdgeSize - 1);
            int          E  = -1;
            while (EdgeSlot[(int)H] >= 0)
            {
                const int Have = EdgeSlot[(int)H];
                if (EdgeA[Have] == Lw && EdgeB[Have] == Hg)
                {
                    E = Have;
                    break;
                }
                H = (H + 1) & (unsigned int)(EdgeSize - 1);
            }
            if (E < 0)
            {
                E                = Edges++;
                EdgeSlot[(int)H] = E;
                EdgeA[E]         = Lw;
                EdgeB[E]         = Hg;
                EdgeC0[E]        = c;
                EdgeC1[E]        = -1;
                Third[E]         = -1;
                EdgeCount[E]     = 0;
            }
            else if (EdgeCount[E] == 1)
                EdgeC1[E] = c;
            else if (EdgeCount[E] == 2)
                Third[E] = c;
            EdgeCount[E]++;
            EdgeOf[c] = E;
        }

        // Each edge's kind - and which inner edges only the places' precision makes inner (ByHair).
        TList<unsigned char> ByHair;
        if (!ByHair.Resize(Edges > 0 ? Edges : 1))
            return false;
        for (int E = 0; E < Edges; E++)
            ByHair[E] = 0;
        for (int E = 0; E < Edges; E++)
        {
            EdgeKind[E] = Edge_Fixed;
            // Three faces: a sheet's, and two in one plane either side of the edge - the edge is theirs, and the
            // sheet goes along with it.
            bool bAlong = false;
            if (Sheet && EdgeCount[E] == 3 && EdgeA[E] != EdgeB[E])
            {
                const int Cs[3] = { EdgeC0[E], EdgeC1[E], Third[E] };
                for (int i = 0; i < 3 && !bAlong; i++)
                {
                    const int j = (i + 1) % 3, k = (i + 2) % 3;
                    const int Fi = Cs[i] / 3, Fj = Cs[j] / 3, Fk = Cs[k] / 3;
                    if (Sheet[Fk] && !Sheet[Fi] && !Sheet[Fj] && Fi != Fj && Drawn(Fi) && Drawn(Fj) &&
                        Dot(FaceN[Fi], FaceN[Fi]) > 0.0f && Dot(FaceN[Fj], FaceN[Fj]) > 0.0f &&
                        Weld[Corner[Cs[i]]] != Weld[Corner[Cs[j]]] && Dot(FaceN[Fi], FaceN[Fj]) >= FlatCos)
                    {
                        EdgeC0[E] = Cs[i];
                        EdgeC1[E] = Cs[j];
                        bAlong    = true;
                        if (!SheetAlong.Add(Fk) || !SheetAlong.Add(Fi) || !SheetAlong.Add(Fj))
                            return false;
                    }
                }
            }
            const int C0 = EdgeC0[E], C1 = EdgeC1[E];
            const int F0 = C0 / 3;
            if (EdgeA[E] == EdgeB[E] || !Drawn(F0) || Dot(FaceN[F0], FaceN[F0]) == 0.0f)
                continue;
            if (EdgeCount[E] == 1)
            {
                if (bOpenFree)
                    EdgeKind[E] = Edge_Open;
                continue;
            }
            if (EdgeCount[E] != 2 && !bAlong)
                continue;
            const int F1 = C1 / 3;
            // The same face twice, a face not drawn, one of no area, or the two turned opposite ways.
            if (F1 == F0 || !Drawn(F1) || Dot(FaceN[F1], FaceN[F1]) == 0.0f || Weld[Corner[C0]] == Weld[Corner[C1]])
                continue;
            const float C     = Dot(FaceN[F0], FaceN[F1]);
            bool        bFlat = C >= FlatCos;
            if (!bFlat && FlatNear > 0.0f && C >= SoftCosine)
            {
                // Each face's far corner, off the other's plane, within the places' precision - only where the two
                // turn softly: a crease is a hard edge however small its faces.
                const FV A  = Pose.At(Weld[Corner[C0]]);
                const FV D0 = Pose.At(Weld[Corner[F0 * 3 + (C0 % 3 + 2) % 3]]) - A;
                const FV D1 = Pose.At(Weld[Corner[F1 * 3 + (C1 % 3 + 2) % 3]]) - A;
                bFlat       = fabsf(Dot(D1, FaceN[F0])) <= FlatNear && fabsf(Dot(D0, FaceN[F1])) <= FlatNear;
                ByHair[E]   = bFlat ? 1 : 0;
            }
            EdgeKind[E] = (unsigned char)(bFlat ? Edge_Inner : (C >= SoftCosine) ? Edge_Soft : Edge_Crease);
        }
#ifdef LEVELROUND_TRACE
        for (int E = 0; E < Edges; E++)
        {
            const FV A = Pose.At(EdgeA[E]), B = Pose.At(EdgeB[E]);
            LevelRoundTraceEdgeKind(E, EdgeKind[E], EdgeCount[E], A.X, A.Y, A.Z, B.X, B.Y, B.Z);
        }
#endif
        // The flat polygons: faces joined across inner edges.
        TList<int> Up;
        if (!Up.Resize(FaceCount * 3))
            return false;
        auto Root = [&](int C)
        {
            while (Up[C] != C)
            {
                Up[C] = Up[Up[C]];
                C     = Up[C];
            }
            return C;
        };
        auto Join = [&](int A, int B)
        {
            A = Root(A);
            B = Root(B);
            if (A != B)
            {
                if (A < B)
                    Up[B] = A;
                else
                    Up[A] = B;
            }
        };
        for (int f = 0; f < FaceCount; f++)
            Up[f] = f;
        for (int E = 0; E < Edges; E++)
            if (EdgeKind[E] == Edge_Inner)
                Join(EdgeC0[E] / 3, EdgeC1[E] / 3);
        for (int f = 0; f < FaceCount; f++)
            Poly[f] = Root(f);
        // A flat polygon joined by the places' precision is one only where all of it lies within that precision of its
        // own plane - its faces' normals and middles weighed by their areas: a small round part of many faces would
        // otherwise join, edge by edge, into one "flat" polygon that is not. Where it does not, the edges so joined go
        // back to soft, and the polygons are joined again.
        if (FlatNear > 0.0f)
        {
            bool bAny = false;
            for (int E = 0; E < Edges && !bAny; E++)
                bAny = ByHair[E] != 0;
            if (bAny)
            {
                TList<FV>    Normal, Middle;
                TList<float> Area, Off;
                if (!Normal.Resize(FaceCount) || !Middle.Resize(FaceCount) || !Area.Resize(FaceCount) ||
                    !Off.Resize(FaceCount))
                    return false;
                for (int f = 0; f < FaceCount; f++)
                {
                    Normal[f] = Middle[f] = V(0.0f, 0.0f, 0.0f);
                    Area[f]   = 0.0f;
                    Off[f]    = 0.0f;
                }
                for (int f = 0; f < FaceCount; f++)
                {
                    const FV    A = Pose.At(Weld[Corner[f * 3]]), B = Pose.At(Weld[Corner[f * 3 + 1]]),
                                C = Pose.At(Weld[Corner[f * 3 + 2]]);
                    const FV    N = Cross(B - A, C - A);
                    const float S = Length(N);
                    const int   R = Poly[f];
                    Normal[R]     = Normal[R] + N;
                    Middle[R]     = Middle[R] + (A + B + C) * (S / 3.0f);
                    Area[R]      += S;
                }
                for (int f = 0; f < FaceCount; f++)
                {
                    const int R = Poly[f];
                    if (!(Area[R] > 0.0f))
                        continue;
                    const FV N = Unit(Normal[R]), At = Middle[R] * (1.0f / Area[R]);
                    for (int k = 0; k < 3; k++)
                        Off[R] = MaxOf(Off[R], fabsf(Dot(Pose.At(Weld[Corner[f * 3 + k]]) - At, N)));
                }
                bool bAgain = false;
                for (int E = 0; E < Edges; E++)
                    if (ByHair[E] && Off[Poly[EdgeC0[E] / 3]] > FlatNear)
                    {
                        EdgeKind[E] = Edge_Soft;
                        bAgain      = true;
                    }
                if (bAgain)
                {
                    for (int f = 0; f < FaceCount; f++)
                        Up[f] = f;
                    for (int E = 0; E < Edges; E++)
                        if (EdgeKind[E] == Edge_Inner)
                            Join(EdgeC0[E] / 3, EdgeC1[E] / 3);
                    for (int f = 0; f < FaceCount; f++)
                        Poly[f] = Root(f);
                }
            }
        }

        // The fans: the corners at one welded vertex joined across each inner and soft edge.
        for (int c = 0; c < FaceCount * 3; c++)
            Up[c] = c;
        for (int E = 0; E < Edges; E++)
        {
            if (EdgeKind[E] != Edge_Inner && EdgeKind[E] != Edge_Soft)
                continue;
            const int C0 = EdgeC0[E], C1 = EdgeC1[E];
            const int F0 = C0 / 3, F1 = C1 / 3;
            // F0's corner at its edge's start with F1's corner after C1 (the same place), and the other way round.
            Join(C0, F1 * 3 + (C1 % 3 + 1) % 3);
            Join(F0 * 3 + (C0 % 3 + 1) % 3, C1);
        }
        TList<int> FanOfRoot;
        if (!FanOfRoot.Resize(FaceCount * 3))
            return false;
        for (int c = 0; c < FaceCount * 3; c++)
            FanOfRoot[c] = -1;
        for (int c = 0; c < FaceCount * 3; c++)
        {
            const int R = Root(c);
            if (FanOfRoot[R] < 0)
                FanOfRoot[R] = Fans++;
            Fan[c] = FanOfRoot[R];
        }
        if (!FanStart.Resize(Fans + 1) || !FanFaces.Resize(FaceCount * 3) || !FanVertex.Resize(Fans) ||
            !FanNormalOf.Resize(Fans) || !FanStampOf.Resize(Fans) || !FaceNormalOf.Resize(FaceCount) ||
            !FaceAngleOf.Resize(FaceCount * 3) || !FaceStampOf.Resize(FaceCount) || !Term.Resize(Edges * EDGE_SLOTS) ||
            !Room.Resize(Edges * EDGE_SLOTS) || !TermStampOf.Resize(Edges) ||
            !OnEdge.Resize(Edges * 2) || !NearEdge.Resize(Edges * 2) || !Bound0.Resize(Edges * 2) ||
            !Bound1.Resize(Edges * 2) ||
            !OutlineA.Resize(Edges) || !OutlineB.Resize(Edges) || !EdgeCap.Resize(Edges))
            return false;
        for (int f = 0; f < FaceCount; f++)
            FaceStampOf[f] = 0;
        for (int c = 0; c < FaceCount * 3; c++)
            FanVertex[Fan[c]] = Weld[Corner[c]];
        for (int i = 0; i <= Fans; i++)
            FanStart[i] = 0;
        for (int c = 0; c < FaceCount * 3; c++)
            FanStart[Fan[c] + 1]++;
        for (int i = 0; i < Fans; i++)
            FanStart[i + 1] += FanStart[i];
        {
            TList<int> Fill;
            if (!Fill.Resize(Fans))
                return false;
            for (int i = 0; i < Fans; i++)
                Fill[i] = FanStart[i];
            for (int c = 0; c < FaceCount * 3; c++)
                FanFaces[Fill[Fan[c]]++] = c / 3;
        }
        for (int i = 0; i < Fans; i++)
            FanStampOf[i] = 0;
        for (int E = 0; E < Edges; E++)
            TermStampOf[E] = 0;

        // The edges at each welded vertex, for the walks below.
        TList<int> VStart, VEdges;
        if (!VStart.Resize(VertCount + 1) || !VEdges.Resize(Edges * 2))
            return false;
        for (int v = 0; v <= VertCount; v++)
            VStart[v] = 0;
        for (int E = 0; E < Edges; E++)
        {
            VStart[EdgeA[E] + 1]++;
            VStart[EdgeB[E] + 1]++;
        }
        for (int v = 0; v < VertCount; v++)
            VStart[v + 1] += VStart[v];
        {
            TList<int> Fill;
            if (!Fill.Resize(VertCount))
                return false;
            for (int v = 0; v < VertCount; v++)
                Fill[v] = VStart[v];
            for (int E = 0; E < Edges; E++)
            {
                VEdges[Fill[EdgeA[E]]++] = E;
                VEdges[Fill[EdgeB[E]]++] = E;
            }
        }
        auto OtherEnd = [&](int E, int v) { return (EdgeA[E] == v) ? EdgeB[E] : EdgeA[E]; };
        auto Way      = [&](int E, int v) { return Unit(Pose.At(OtherEnd(E, v)) - Pose.At(v)); };
        auto IsTrue   = [&](int E)
        { return EdgeKind[E] == Edge_Soft || EdgeKind[E] == Edge_Crease || EdgeKind[E] == Edge_Open; };

        // At each end of a true edge, the true edge that carries the curve on past it - the most nearly straight on, a
        // turn of the chain no sharper than two faces of one surface may turn by - and the shortest other true edge.
        for (int E = 0; E < Edges; E++)
            for (int End = 0; End < 2; End++)
            {
                OnEdge[E * 2 + End] = NearEdge[E * 2 + End] = -1;
                if (!IsTrue(E))
                    continue;
                const int v     = End ? EdgeB[E] : EdgeA[E];
                const FV  Here  = Way(E, v);
                float     Best  = SoftCosine;
                float     Least = 0.0f;
                for (int i = VStart[v]; i < VStart[v + 1]; i++)
                {
                    const int G = VEdges[i];
                    if (G == E || !IsTrue(G))
                        continue;
                    const float Straight = -Dot(Here, Way(G, v));
                    if (Straight >= Best)
                    {
                        Best                = Straight;
                        OnEdge[E * 2 + End] = G;
                    }
                    const float L = Length(Pose.At(EdgeB[G]) - Pose.At(EdgeA[G]));
                    if (NearEdge[E * 2 + End] < 0 || L < Least)
                    {
                        Least                 = L;
                        NearEdge[E * 2 + End] = G;
                    }
                }
            }

        // The true edges on closed rings. A way along a true edge (E * 2 + 1 towards its end B, E * 2 towards A) has one
        // way on, along the edge that carries the curve on past that end: each walk from a way not yet walked ends at one
        // walked before, or comes back to one of its own - every edge from there round is on a ring.
        {
            TList<int> WalkOf;   // a way: the walk that met it (-1 none)
            if (!OnRing.Resize(Edges) || !WalkOf.Resize(Edges * 2))
                return false;
            for (int E = 0; E < Edges; E++)
                OnRing[E] = 0;
            for (int W = 0; W < Edges * 2; W++)
                WalkOf[W] = -1;
            auto WayOn = [&](int W)
            {
                const int E = W / 2, v = (W & 1) ? EdgeB[E] : EdgeA[E], G = OnEdge[E * 2 + (W & 1)];
                return (G < 0) ? -1 : G * 2 + ((EdgeA[G] == v) ? 1 : 0);
            };
            for (int W0 = 0; W0 < Edges * 2; W0++)
            {
                if (WalkOf[W0] >= 0 || !IsTrue(W0 / 2))
                    continue;
                int W = W0;
                while (W >= 0 && WalkOf[W] < 0)
                {
                    WalkOf[W] = W0;
                    W         = WayOn(W);
                }
                if (W >= 0 && WalkOf[W] == W0)
                    for (int X = W;;)
                    {
                        OnRing[X / 2] = 1;
                        X             = WayOn(X);
                        if (X == W)
                            break;
                    }
            }
        }

        // At each end of an inner edge, the two true edges that bound its polygon there: from each of its faces, round
        // the vertex away from the edge through the polygon's faces, to the first edge that is not an inner one.
        auto CornerAt = [&](int f, int v)
        {
            for (int k = 0; k < 3; k++)
                if (Weld[Corner[f * 3 + k]] == v)
                    return k;
            return -1;
        };
        auto FaceAcross = [&](int E, int f)
        {
            if (EdgeCount[E] != 2)
                return -1;
            return (EdgeC0[E] / 3 == f) ? EdgeC1[E] / 3 : EdgeC0[E] / 3;
        };
        auto BoundFrom = [&](int E, int f, int v)
        {
            int Through = E;
            for (int Step = 0; Step < 64 && f >= 0; Step++)
            {
                const int k = CornerAt(f, v);
                if (k < 0)
                    return -1;
                // f's two edges at v: the one from v (corner k) and the one into v (corner k - 1).
                const int Out = EdgeOf[f * 3 + k], In = EdgeOf[f * 3 + (k + 2) % 3];
                const int G   = (Out == Through) ? In : Out;
                if (G == Through || G == E)
                    return -1;
                if (EdgeKind[G] != Edge_Inner)
                    return IsTrue(G) ? G : -1;
                Through = G;
                f       = FaceAcross(G, f);
            }
            return -1;
        };
        for (int E = 0; E < Edges; E++)
            for (int End = 0; End < 2; End++)
            {
                Bound0[E * 2 + End] = Bound1[E * 2 + End] = -1;
                if (EdgeKind[E] != Edge_Inner)
                    continue;
                const int v         = End ? EdgeB[E] : EdgeA[E];
                Bound0[E * 2 + End] = BoundFrom(E, EdgeC0[E] / 3, v);
                Bound1[E * 2 + End] = BoundFrom(E, EdgeC1[E] / 3, v);
            }

        // An open edge's neighbours along the outline: at each end, the one other open edge there.
        for (int E = 0; E < Edges; E++)
        {
            OutlineA[E] = OutlineB[E] = -1;
            if (EdgeKind[E] != Edge_Open)
                continue;
            for (int End = 0; End < 2; End++)
            {
                const int v     = End ? EdgeB[E] : EdgeA[E];
                int       Found = -1, Count = 0;
                for (int i = VStart[v]; i < VStart[v + 1]; i++)
                {
                    const int G = VEdges[i];
                    if (G != E && EdgeKind[G] == Edge_Open)
                    {
                        Found = G;
                        Count++;
                    }
                }
                (End ? OutlineB[E] : OutlineA[E]) = (Count == 1) ? Found : -1;
            }
        }

        // The outline edges of each flat polygon (by its lowest face), for how deep it reaches across from one of them
        // (EdgeCapOf, worked out when an edge's terms first are).
        if (!PolyEdgeStart.Resize(FaceCount + 1) || !PolyEdgeList.Resize(Edges * 2))
            return false;
        for (int f = 0; f <= FaceCount; f++)
            PolyEdgeStart[f] = 0;
        auto SideOf = [&](int E, int k) { return (k == 0) ? EdgeC0[E] : EdgeC1[E]; };
        for (int E = 0; E < Edges; E++)
        {
            EdgeCap[E] = -1.0f;
            if (EdgeKind[E] != Edge_Inner)
                for (int k = 0; k < 2; k++)
                    if (SideOf(E, k) >= 0)
                        PolyEdgeStart[Poly[SideOf(E, k) / 3] + 1]++;
        }
        for (int f = 0; f < FaceCount; f++)
            PolyEdgeStart[f + 1] += PolyEdgeStart[f];
        {
            TList<int> Fill;
            if (!Fill.Resize(FaceCount))
                return false;
            for (int f = 0; f < FaceCount; f++)
                Fill[f] = PolyEdgeStart[f];
            for (int E = 0; E < Edges; E++)
                if (EdgeKind[E] != Edge_Inner)
                    for (int k = 0; k < 2; k++)
                        if (SideOf(E, k) >= 0)
                            PolyEdgeList[Fill[Poly[SideOf(E, k) / 3]]++] = E;
        }
        Stamp = 0;
        return true;
    }

    // The outline of face Face's flat polygon where it is one of several faces - more outline edges than a triangle has,
    // and no more than Most: its edges, into Out, and how many; none for a polygon of one face, or of more edges
    // (PolyPlaneMoveAt).
    int PolyOutline(int Face, int* Out, int Most) const
    {
        const int Lowest = Poly[Face];
        const int First = PolyEdgeStart[Lowest], Num = PolyEdgeStart[Lowest + 1] - First;
        if (Num <= 3 || Num > Most)
            return 0;
        for (int i = 0; i < Num; i++)
            Out[i] = PolyEdgeList[First + i];
        return Num;
    }

    // The plane of face Face in the draw's pose: its normal, of length one - none for a face of no area.
    FV PlaneOfFace(int Face) const
    {
        const int* const C = M->Corner.Data + Face * 3;
        const FV         A = P.At(Weld[C[0]]);
        return Unit(Cross(P.At(Weld[C[1]]) - A, P.At(Weld[C[2]]) - A));
    }

    // A draw's pose, until the next: NewStamp, a number no earlier pose of this shape had.
    void Pose(const FPose& Places, int NewStamp)
    {
        P     = Places;
        Stamp = NewStamp;
        bFit  = false;
    }

    // Face f's unit normal and corner angles in the draw's pose (zero for a face of no area).
    void FaceOf(int f)
    {
        if (FaceStampOf[f] == Stamp)
            return;
        const int* const Corner = M->Corner.Data;
        const FV         A      = P.At(Weld[Corner[f * 3]]);
        const FV         B      = P.At(Weld[Corner[f * 3 + 1]]);
        const FV         C      = P.At(Weld[Corner[f * 3 + 2]]);
        const FV         AB = B - A, BC = C - B, CA = A - C;
        const FV         N  = Cross(AB, V(0.0f, 0.0f, 0.0f) - CA);
        const float      L  = Length(N);
        if (L > 1e-12f)
        {
            FaceNormalOf[f]        = N * (1.0f / L);
            FaceAngleOf[f * 3]     = AngleOf(L, -Dot(CA, AB));
            FaceAngleOf[f * 3 + 1] = AngleOf(L, -Dot(AB, BC));
            FaceAngleOf[f * 3 + 2] = AngleOf(L, -Dot(BC, CA));
        }
        else
        {
            FaceNormalOf[f]    = V(0.0f, 0.0f, 0.0f);
            FaceAngleOf[f * 3] = FaceAngleOf[f * 3 + 1] = FaceAngleOf[f * 3 + 2] = 0.0f;
        }
        FaceStampOf[f] = Stamp;
    }

    FV FanNormal(int F)
    {
        if (FanStampOf[F] == Stamp)
            return FanNormalOf[F];
        const int* const Corner = M->Corner.Data;
        FV               Sum    = V(0.0f, 0.0f, 0.0f);
        const int        W      = FanVertex[F];
        for (int i = FanStart[F]; i < FanStart[F + 1]; i++)
        {
            const int f = FanFaces[i];
            int       k = 0;
            while (k < 2 && Weld[Corner[f * 3 + k]] != W)
                k++;
            FaceOf(f);
            Sum = Sum + FaceNormalOf[f] * FaceAngleOf[f * 3 + k];
        }
        FanNormalOf[F] = Unit(Sum);
        FanStampOf[F]  = Stamp;
        return FanNormalOf[F];
    }

    // The fan of face corner C's face at its welded vertex v.
    int FanAt(int C, int v) const
    {
        const int        f      = C / 3;
        const int* const Corner = M->Corner.Data;
        for (int k = 0; k < 3; k++)
            if (Weld[Corner[f * 3 + k]] == v)
                return Fan[f * 3 + k];
        return -1;
    }

    // The way a true edge E leaves its end v along the surface (Out: the edge from v to its other end) - unit, or zero
    // where it stays straight there - and in Most the sine of the furthest E's start may turn onto it:
    // - a soft edge: where a true edge carries the curve on past v within a soft turn, the way between the two (into v
    //   along that one, out of v along E), each weighed by its length - a ring round a can follows its circle however its
    //   corners' fans lean towards a lid, and where a long edge goes on into a short one (a bottle's side into its
    //   shoulder), the long one stays straight and the short one does the turning; else E's own way laid into the fan's
    //   plane;
    // - a crease: along the crease's own line (the two fans' normals crossed), where E's way strays from it by no more
    //   than an end may bend;
    // - an open edge: between the open edge before it and E, where the outline turns softly there, as a soft edge's.
    FV EndWay(int E, int v, FV Out, float& Most)
    {
        Most = EndSine;
        const int   Kind = EdgeKind[E];
        const float EL   = Length(Out);
        if (!(EL > 1e-6f))
            return V(0.0f, 0.0f, 0.0f);
        const FV OutWay = Out * (1.0f / EL);
        // Out's way off a line T, held to an end's most: zero past it.
        auto Within = [&](FV T)
        {
            const float Off = Length(OutWay - T * Dot(OutWay, T));
            return (Off <= EndSine && Dot(OutWay, T) > 0.0f) ? T : V(0.0f, 0.0f, 0.0f);
        };
        if (Kind == Edge_Soft)
        {
            const int G = OnEdge[(v == EdgeA[E]) ? E * 2 : E * 2 + 1];
            if (G >= 0)
            {
                const FV Into = P.At(v) - P.At((EdgeA[G] == v) ? EdgeB[G] : EdgeA[G]);
                if (Dot(Unit(Into), OutWay) >= SoftCosine)
                {
                    Most = FullSine;
                    return Unit(Into + Out);
                }
            }
            const FV N = FanNormal(FanAt(EdgeC0[E], v));
            return Unit(OutWay - N * Dot(OutWay, N));
        }
        if (Kind == Edge_Crease)
        {
            const int Fa = FanAt(EdgeC0[E], v), Fb = FanAt(EdgeC1[E], v);
            if (Fa < 0 || Fb < 0)
                return V(0.0f, 0.0f, 0.0f);
            const int Lw = (Fa < Fb) ? Fa : Fb;
            const int Hg = (Fa < Fb) ? Fb : Fa;
            FV        T  = Cross(FanNormal(Lw), FanNormal(Hg));
            if (!(Length(T) > 1e-4f))
                return V(0.0f, 0.0f, 0.0f);
            T = Unit(T);
            return Within((Dot(T, OutWay) < 0.0f) ? V(0.0f, 0.0f, 0.0f) - T : T);
        }
        if (Kind == Edge_Open)
        {
            const int Other = (v == EdgeA[E]) ? OutlineA[E] : OutlineB[E];
            if (Other < 0)
                return V(0.0f, 0.0f, 0.0f);
            const FV Into = P.At(v) - P.At((EdgeA[Other] == v) ? EdgeB[Other] : EdgeA[Other]);
            if (Dot(Unit(Into), OutWay) < SoftCosine)
                return V(0.0f, 0.0f, 0.0f);
            Most = FullSine;
            return Unit(Into + Out);
        }
        return V(0.0f, 0.0f, 0.0f);
    }

    // An end's term from the way it leaves along (EndWay) and the edge out of it: what turns the edge's start onto that
    // way, held to Most (a sine) of the edge.
    FV TermOf(FV Way, FV Out, float Most) const
    {
        if (Dot(Way, Way) == 0.0f)
            return V(0.0f, 0.0f, 0.0f);
        FV          T   = Way * Dot(Out, Way) - Out;
        const float L   = Length(T);
        const float Top = Length(Out) * Most;
        if (L > Top)
            T = T * (Top / L);
        return T;
    }

    // The way into face corner C's face from its edge E, in the face's plane, of length one: towards the face's corner
    // off the edge (C: the corner E starts at in that face).
    FV IntoFace(int C, int E)
    {
        const int f = C / 3;
        FaceOf(f);
        const FV A  = P.At(EdgeA[E]);
        FV       In = Cross(FaceNormalOf[f], Unit(P.At(EdgeB[E]) - A));
        if (Dot(In, P.At(Weld[M->Corner.Data[f * 3 + (C % 3 + 2) % 3]]) - A) < 0.0f)
            In = V(0.0f, 0.0f, 0.0f) - In;
        return In;
    }

    // Whether true edge G, met across face f's flat polygon from true edge E (Bow: the way E bows, of length one), is
    // where something stands out of that polygon's front while E's other polygon lies behind it, E bowing along the
    // polygon's plane - a table's rim over the leg under it: E passing G only folds the polygon between them along its
    // own plane, and what stands out stays clear of what lies behind.
    bool StandsOut(int E, int f, int G, const FV& Bow)
    {
        if (G < 0 || G == E || EdgeC0[E] < 0 || EdgeC1[E] < 0 || EdgeC0[G] < 0 || EdgeC1[G] < 0)
            return false;
        FaceOf(f);
        const FV  Np = FaceNormalOf[f];
        const int r  = Poly[f];
        if (fabsf(Dot(Bow, Np)) > 0.5f)
            return false;
        // E's other face, and G's off this polygon.
        const int CE = (EdgeC0[E] / 3 == f) ? EdgeC1[E] : EdgeC0[E];
        const int CG = (Poly[EdgeC0[G] / 3] == r) ? EdgeC1[G] : (Poly[EdgeC1[G] / 3] == r) ? EdgeC0[G] : -1;
        if (CG < 0 || Poly[CE / 3] == r || Poly[CG / 3] == r)
            return false;
        return Dot(IntoFace(CE, E), Np) < -0.5f && Dot(IntoFace(CG, G), Np) > 0.5f;
    }

    // EdgeCap of true edge E, worked out once: how deep each of its two flat polygons reaches across from it - from three
    // places along it, across that polygon's plane into it, to the first other edge of its outline met -, a quarter of
    // the shallower, none past Cap. With bRingPasses, a polygon whose edge met there is where something stands out of it
    // (StandsOut) is no bound for an edge of a closed ring: a ring rounds whole, not held flat where a leg is under it.
    float EdgeCapOf(int E)
    {
        if (EdgeCap[E] >= 0.0f)
            return EdgeCap[E];
        EdgeCap[E]       = Cap;
        const FV    A    = P.At(EdgeA[E]), B = P.At(EdgeB[E]);
        const float L    = Length(B - A);
        if (!(L > 1e-6f))
            return EdgeCap[E];
        const FV         D      = (B - A) * (1.0f / L);
        const int* const Corner = M->Corner.Data;
        // The way the edge bows - at its start, or else its end - where it may pass what stands out beside it.
        FV Bow = V(0.0f, 0.0f, 0.0f);
        if (bRingPasses && OnRing[E])
        {
            float Most = 0.0f;
            Bow        = TermOf(EndWay(E, EdgeA[E], B - A, Most), B - A, 1.0f);
            if (!(Length(Bow) > 1e-6f))
                Bow = TermOf(EndWay(E, EdgeB[E], A - B, Most), A - B, 1.0f);
            Bow = Unit(Bow);
        }
        for (int k = 0; k < 2; k++)
        {
            const int C = (k == 0) ? EdgeC0[E] : EdgeC1[E];
            if (C < 0)
                continue;
            const int f = C / 3, r = Poly[f];
            FaceOf(f);
            FV In = Cross(FaceNormalOf[f], D);
            // Towards the face's third corner.
            if (Dot(In, P.At(Weld[Corner[f * 3 + (C % 3 + 2) % 3]]) - A) < 0.0f)
                In = V(0.0f, 0.0f, 0.0f) - In;
            if (!(Dot(In, In) > 0.5f))
                continue;
            float Deep     = 1e30f;
            int   DeepEdge = -1;
            for (int s = 1; s <= 3; s++)
            {
                const FV Mid = A + D * (L * 0.25f * (float)s);
                for (int i = PolyEdgeStart[r]; i < PolyEdgeStart[r + 1]; i++)
                {
                    const int G = PolyEdgeList[i];
                    if (G == E)
                        continue;
                    const FV    P0 = P.At(EdgeA[G]) - Mid, P1 = P.At(EdgeB[G]) - Mid;
                    const float X0 = Dot(P0, D), X1 = Dot(P1, D);
                    if ((X0 > 0.0f && X1 > 0.0f) || (X0 < 0.0f && X1 < 0.0f) || X0 == X1)
                        continue;
                    const float Y = Dot(P0, In) + X0 / (X0 - X1) * (Dot(P1, In) - Dot(P0, In));
                    if (Y > 1e-4f * L && Y < Deep)
                    {
                        Deep     = Y;
                        DeepEdge = G;
                    }
                }
            }
            if (Dot(Bow, Bow) > 0.5f && StandsOut(E, f, DeepEdge, Bow))
                continue;
            if (Deep < 1e29f && 0.25f * Deep < EdgeCap[E])
                EdgeCap[E] = 0.25f * Deep;
        }
        return EdgeCap[E];
    }

    // The room a true edge's bend takes at its end (End: 0 at A, 1 at B), as a share of the edge (Long): within the edge
    // that carries the curve on past it where that one is shorter - where none does, the shortest true edge there -
    // and within the length that bends it out by at most Most - the edge's EdgeCapOf (Sine: how far the end bends, as a sine).
    float RoomOf(int E, int End, float Long, float Sine, float Most) const
    {
        float     Share = 1.0f;
        const int G     = (OnEdge[E * 2 + End] >= 0) ? OnEdge[E * 2 + End] : NearEdge[E * 2 + End];
        if (G >= 0)
        {
            const float L = Length(P.At(EdgeB[G]) - P.At(EdgeA[G]));
            if (L < Share * Long)
                Share = L / Long;
        }
        if (Sine > 1e-6f)
        {
            const float Capped = Most / (BendTop * Sine);
            if (Capped < Share * Long)
                Share = Capped / Long;
        }
        return (Share < TauLeast) ? TauLeast : Share;
    }

    // Edge E's terms in the draw's pose: at each end, what turns the edge's start out of that end onto the way it leaves
    // along - the same vector whichever way along the edge it is asked for - and the room it is done within.
    void Terms(int E)
    {
        if (TermStampOf[E] == Stamp)
            return;
        TermStampOf[E] = Stamp;
        FV* const    T = &Term[E * EDGE_SLOTS];
        float* const R = &Room[E * EDGE_SLOTS];
        for (int k = 0; k < EDGE_SLOTS; k++)
        {
            T[k] = V(0.0f, 0.0f, 0.0f);
            R[k] = 1.0f;
        }
        const int Kind = EdgeKind[E];
        if (Kind == Edge_Fixed)
            return;
        const FV    PA = P.At(EdgeA[E]), PB = P.At(EdgeB[E]);
        const FV    Along = PB - PA;
        const float Long  = Length(Along);
        if (!(Long > 1e-6f))
            return;
        const FV Back = V(0.0f, 0.0f, 0.0f) - Along;
        if (Kind != Edge_Inner)
        {
            float    MostA = EndSine, MostB = EndSine;
            const FV WayA  = EndWay(E, EdgeA[E], Along, MostA);
            const FV WayB  = EndWay(E, EdgeB[E], Back, MostB);
            T[0]           = TermOf(WayA, Along, MostA);
            T[2]           = TermOf(WayB, Back, MostB);
            const float Most = EdgeCapOf(E);
            R[0]             = RoomOf(E, 0, Long, Length(T[0]) / Long, Most);
            R[2]             = RoomOf(E, 1, Long, Length(T[2]) / Long, Most);
#ifdef LEVELROUND_TRACE
            LevelRoundTraceEdge(Kind, PA.X, PA.Y, PA.Z, PB.X, PB.Y, PB.Z, Length(WayA), Length(WayB), Length(T[0]),
                                Length(T[2]), Most, R[0], R[2]);
#endif
            return;
        }
        // An inner edge: at each end, its way told apart along the two true edges that bound its polygon there, and each
        // of those ways turned as that edge's own start turns there - two bends, each within the share of this edge that
        // reaches as far along that edge's way as its own bend does: the polygon goes along with its edges.
        for (int End = 0; End < 2; End++)
        {
            const int v   = End ? EdgeB[E] : EdgeA[E];
            const FV  Dir = (End ? Back : Along) * (1.0f / Long);
            const int G0 = Bound0[E * 2 + End], G1 = Bound1[E * 2 + End];
            FV        W0 = V(0.0f, 0.0f, 0.0f), W1 = V(0.0f, 0.0f, 0.0f), D0 = W0, D1 = W0;
            float     L0 = 0.0f, L1 = 0.0f, R0 = 1.0f, R1 = 1.0f;
            // A bound's way out of v, how its start turns there (its term at v over its length), and how far along it
            // the turn is done (its room, in units).
            auto Bound = [&](int G, FV& W, FV& D, float& GL, float& GR)
            {
                const FV Out = P.At((EdgeA[G] == v) ? EdgeB[G] : EdgeA[G]) - P.At(v);
                GL           = Length(Out);
                if (!(GL > 1e-6f))
                    return;
                Terms(G);
                const int Slot = (EdgeA[G] == v) ? 0 : 2;
                W              = Out * (1.0f / GL);
                D              = Term[G * EDGE_SLOTS + Slot] * (1.0f / GL);
                GR             = Room[G * EDGE_SLOTS + Slot] * GL;
            };
            if (G0 >= 0)
                Bound(G0, W0, D0, L0, R0);
            if (G1 >= 0)
                Bound(G1, W1, D1, L1, R1);
            // Dir = a W0 + b W1 by least squares; the two bounds in one line, along it alone.
            const float A00 = Dot(W0, W0), A01 = Dot(W0, W1), A11 = Dot(W1, W1);
            const float Det = A00 * A11 - A01 * A01;
            float       a = 0.0f, b = 0.0f;
            if (Det > 1e-4f)
            {
                a = (A11 * Dot(Dir, W0) - A01 * Dot(Dir, W1)) / Det;
                b = (A00 * Dot(Dir, W1) - A01 * Dot(Dir, W0)) / Det;
            }
            else if (A00 > 0.5f)
                a = Dot(Dir, W0);
            else if (A11 > 0.5f)
                b = Dot(Dir, W1);
            // Each bend within the share of this edge whose run along the bound's way is the bound's room: this edge goes
            // a (or b) of its length along that way.
            const float Most = Long * EndSine;
            FV          T0   = D0 * (a * Long), T1 = D1 * (b * Long);
            const float L0w = Length(T0), L1w = Length(T1);
            if (L0w > Most)
                T0 = T0 * (Most / L0w);
            if (L1w > Most)
                T1 = T1 * (Most / L1w);
            const int Slot = End ? 2 : 0;
            T[Slot]        = T0;
            T[Slot + 1]    = T1;
            R[Slot]        = (fabsf(a) * Long > R0) ? R0 / (fabsf(a) * Long) : 1.0f;
            R[Slot + 1]    = (fabsf(b) * Long > R1) ? R1 / (fabsf(b) * Long) : 1.0f;
            R[Slot]        = (R[Slot] < TauLeast) ? TauLeast : R[Slot];
            R[Slot + 1]    = (R[Slot + 1] < TauLeast) ? TauLeast : R[Slot + 1];
        }
    }

    // Every edge's terms in the draw's pose worked out ahead (Terms), on threads (RdxSphere::OnThreads), layer by layer:
    // what an edge's terms read - its faces' normals and angles (FaceOf), its fans' normals (FanNormal), how deep its
    // polygons reach (EdgeCapOf) -, then the edges that are not inner, then the inner ones, which read the true edges
    // bounding them. Each is worked out by the very function that works it out when it is asked for, from the same
    // values: what is asked for afterwards reads the same, and nothing is written while the threads read.
    void TermsAll(int Workers)
    {
        if (bFit)
            return;
        RdxSphere::OnThreads(M->Faces, 512, Workers, [&](int f, int) { FaceOf(f); });
        RdxSphere::OnThreads(Fans, 512, Workers, [&](int F, int) { FanNormal(F); });
        RdxSphere::OnThreads(Edges, 256, Workers, [&](int E, int)
        {
            const int Kind = EdgeKind[E];
            if (Kind != Edge_Fixed && Kind != Edge_Inner && Length(P.At(EdgeB[E]) - P.At(EdgeA[E])) > 1e-6f)
                EdgeCapOf(E);
        });
        RdxSphere::OnThreads(Edges, 256, Workers, [&](int E, int)
        {
            if (EdgeKind[E] != Edge_Inner)
                Terms(E);
        });
        RdxSphere::OnThreads(Edges, 256, Workers, [&](int E, int)
        {
            if (EdgeKind[E] == Edge_Inner)
                Terms(E);
        });
    }

    // The terms of face Face's edge from its corner Edge to the next, that way along it.
    FEdgeTerms EdgeTerms(int Face, int Edge)
    {
        const int  C = Face * 3 + Edge;
        FEdgeTerms Canon;
        EdgeData(EdgeOf[C], Canon);
        if (Weld[M->Corner[C]] == EdgeA[EdgeOf[C]])
            return Canon;
        FEdgeTerms R;
        for (int k = 0; k < EDGE_SLOTS; k++)
        {
            R.T[k]    = Canon.T[(k + 2) % EDGE_SLOTS];
            R.Room[k] = Canon.Room[(k + 2) % EDGE_SLOTS];
        }
        return R;
    }

    // Edge E's terms from its end A to its end B, in the draw's pose: worked out, or the rest's turned where the pose fits
    // it (Fit). What a patch reads of an edge (SolidPass.hlsl's MeshEdge), from either face of it.
    void EdgeData(int E, FEdgeTerms& Out)
    {
        if (!bFit)
            Terms(E);
        for (int k = 0; k < EDGE_SLOTS; k++)
        {
            if (bFit)
            {
                const FV A  = RestTerm[E * EDGE_SLOTS + k];
                Out.T[k]    = V(Turn[0] * A.X + Turn[1] * A.Y + Turn[2] * A.Z, Turn[3] * A.X + Turn[4] * A.Y +
                                Turn[5] * A.Z, Turn[6] * A.X + Turn[7] * A.Y + Turn[8] * A.Z);
                Out.Room[k] = RestRoom[E * EDGE_SLOTS + k];
            }
            else
            {
                Out.T[k]    = Term[E * EDGE_SLOTS + k];
                Out.Room[k] = Room[E * EDGE_SLOTS + k];
            }
        }
    }

    // Whether any edge bends in the shape's pose: a model all of flat parts is drawn as it always was.
    bool AnyBow()
    {
        for (int E = 0; E < Edges; E++)
        {
            Terms(E);
            for (int k = 0; k < EDGE_SLOTS; k++)
                if (Length(Term[E * EDGE_SLOTS + k]) > 1e-3f)
                    return true;
        }
        return false;
    }

    // The rest, from the pose at hand (Pose): every edge's terms, and every welded vertex's place to hold the next poses
    // against.
    void MakeRest()
    {
        bRest = bFit = false;
        if (!RestTerm.Resize(Edges * EDGE_SLOTS) || !RestRoom.Resize(Edges * EDGE_SLOTS))
            return;
        for (int E = 0; E < Edges; E++)
        {
            Terms(E);
            for (int k = 0; k < EDGE_SLOTS; k++)
            {
                RestTerm[E * EDGE_SLOTS + k] = Term[E * EDGE_SLOTS + k];
                RestRoom[E * EDGE_SLOTS + k] = Room[E * EDGE_SLOTS + k];
            }
        }
        RestRef.Clear();
        RestAt.Clear();
        for (int v = 0; v < M->Verts; v++)
            if (Weld[v] == v && M->VertStart[v] < M->VertStart[v + 1])
            {
                if (!RestRef.Add(v) || !RestAt.Add(P.At(v)))
                    return;
            }
        if (RestRef.Num < 4)
            return;
        // The turn is taken from the two furthest apart of the extremes each way along the axes, and the vertex
        // furthest off their line.
        int Pick[6];
        for (int k = 0; k < 6; k++)
            Pick[k] = 0;
        for (int i = 1; i < RestRef.Num; i++)
        {
            const float A[3] = { RestAt[i].X, RestAt[i].Y, RestAt[i].Z };
            for (int k = 0; k < 6; k++)
            {
                const float B[3] = { RestAt[Pick[k]].X, RestAt[Pick[k]].Y, RestAt[Pick[k]].Z };
                if ((k & 1) ? A[k / 2] < B[k / 2] : A[k / 2] > B[k / 2])
                    Pick[k] = i;
            }
        }
        float Far = 0.0f;
        for (int i = 0; i < 6; i++)
            for (int j = i + 1; j < 6; j++)
            {
                const float D = Length(RestAt[Pick[j]] - RestAt[Pick[i]]);
                if (D > Far)
                {
                    Far   = D;
                    RestA = Pick[i];
                    RestB = Pick[j];
                }
            }
        const FV Line = RestAt[RestB] - RestAt[RestA];
        float    Off  = 0.0f;
        for (int i = 0; i < RestRef.Num; i++)
        {
            const float D = Length(Cross(Line, RestAt[i] - RestAt[RestA]));
            if (D > Off)
            {
                Off   = D;
                RestC = i;
            }
        }
        RestSize = Far;
        bRest    = Far > 1e-3f && Off > 1e-3f * Far * Far;
    }

    // An orthonormal frame from two directions: its three axes, by rows.
    static void FrameOf(FV U, FV W, float* Rows)
    {
        const FV X = U * (1.0f / Length(U));
        FV       Z = Cross(U, W);
        Z          = Z * (1.0f / Length(Z));
        const FV Y    = Cross(Z, X);
        const FV R[3] = { X, Y, Z };
        for (int k = 0; k < 3; k++)
        {
            Rows[k * 3]     = R[k].X;
            Rows[k * 3 + 1] = R[k].Y;
            Rows[k * 3 + 2] = R[k].Z;
        }
    }

    // Whether the pose at hand (Pose) is the rest turned, scaled and moved: every welded vertex stands where the rest's
    // would so moved, within a thousandth of the model's size - a part that moves in itself (a crossbow's string, a jaw)
    // shows. Then EdgeTerms takes the rest's terms, turned.
    bool Fit()
    {
        bFit = false;
        if (!bRest)
            return false;
        const FV    A0 = RestAt[RestA], A1 = P.At(RestRef[RestA]);
        const FV    U0 = RestAt[RestB] - A0, U1 = P.At(RestRef[RestB]) - A1;
        const FV    W0 = RestAt[RestC] - A0, W1 = P.At(RestRef[RestC]) - A1;
        const float L0 = Length(U0), L1 = Length(U1);
        if (!(L0 > 0.0f && L1 > 0.0f) || !(Length(Cross(U1, W1)) > 1e-3f * L1 * L1))
            return false;
        const float Scale = L1 / L0;
        float       F0[9], F1[9];
        FrameOf(U0, W0, F0);
        FrameOf(U1, W1, F1);
        // Turn = Scale * F1^T * F0: a rest direction's parts along the rest's axes, put along the pose's.
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                Turn[r * 3 + c] = Scale * (F1[r] * F0[c] + F1[3 + r] * F0[3 + c] + F1[6 + r] * F0[6 + c]);
        const float Most = 1e-3f * RestSize * Scale;
        for (int i = 0; i < RestRef.Num; i++)
        {
            const FV D = RestAt[i] - A0;
            const FV Q = A1 + V(Turn[0] * D.X + Turn[1] * D.Y + Turn[2] * D.Z, Turn[3] * D.X + Turn[4] * D.Y +
                                Turn[5] * D.Z, Turn[6] * D.X + Turn[7] * D.Y + Turn[8] * D.Z);
            if (!(Length(Q - P.At(RestRef[i])) <= Most))
                return false;
        }
        bFit = true;
        return true;
    }
};

}   // namespace RdxMeshRound

// SecondRevision - the spherifier's common ground (SpherifierDistance=<units>): what the level's rounding (LevelRound.h)
// is handed - the level's polygons, each with its plane and whether it takes part - and what it hands back: every place
// where it was and where it is now, each moved polygon's triangles, what is needed to move a point laid on one, and the
// sides along which the light maps are blended. The small things both sides use with them: a point of three doubles and
// a growing list of plain data.
//
// Engine-independent, as RenderMath.h: the renderer, the unit tests and tools\LevelRoundCheck run the same code.
#pragma once

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
// The engine's headers (UnFile.h) make macros of clock, check and verify - the C library's clock among them, whose
// declaration the threads' header reads: put by meanwhile, and back after.
#pragma push_macro("clock")
#pragma push_macro("check")
#pragma push_macro("verify")
#undef clock
#undef check
#undef verify
#include <atomic>
#include <thread>
#pragma pop_macro("verify")
#pragma pop_macro("check")
#pragma pop_macro("clock")

namespace RdxSphere
{

static const double Pi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------------------------
// Small things
// ---------------------------------------------------------------------------------------------------------------

struct FV3
{
    double X, Y, Z;
};
inline FV3 V3(double X, double Y, double Z)
{
    FV3 V = { X, Y, Z };
    return V;
}
inline FV3 operator+(FV3 A, FV3 B) { return V3(A.X + B.X, A.Y + B.Y, A.Z + B.Z); }
inline FV3 operator-(FV3 A, FV3 B) { return V3(A.X - B.X, A.Y - B.Y, A.Z - B.Z); }
inline FV3 operator*(FV3 A, double S) { return V3(A.X * S, A.Y * S, A.Z * S); }
inline double Dot(FV3 A, FV3 B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
inline FV3 Cross(FV3 A, FV3 B) { return V3(A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X); }
inline double Length(FV3 A) { return sqrt(Dot(A, A)); }

// An angle brought into -pi .. pi.
inline double Wrap(double A) { return A - 2.0 * Pi * floor((A + Pi) / (2.0 * Pi)); }

// The lesser and the greater of two numbers, to the bit as the C library's fmin and fmax (fminf, fmaxf) give them - of
// two zeros the negative one and the positive one, of a NaN and a number the number -, but inline: the library's are
// calls, made for every point the rounding looks at.
inline bool SignOf(double X)
{
    unsigned long long B;
    memcpy(&B, &X, sizeof(B));
    return (B >> 63) != 0;
}
inline bool SignOf(float X)
{
    unsigned int B;
    memcpy(&B, &X, sizeof(B));
    return (B >> 31) != 0;
}
template <typename T> inline T LesserOf(T X, T Y)
{
    if (X < Y)
        return X;
    if (Y < X)
        return Y;
    if (X == Y)
        return SignOf(X) ? X : Y;
    return (X != X) ? Y : X;
}
template <typename T> inline T GreaterOf(T X, T Y)
{
    if (Y < X)
        return X;
    if (X < Y)
        return Y;
    if (X == Y)
        return SignOf(X) ? Y : X;
    return (X != X) ? Y : X;
}
// Two of a kind only, as the library's are: a float and a double, or a whole number, stop the build here.
inline double MinOf(double X, double Y) { return LesserOf(X, Y); }
inline float  MinOf(float X, float Y) { return LesserOf(X, Y); }
inline double MaxOf(double X, double Y) { return GreaterOf(X, Y); }
inline float  MaxOf(float X, float Y) { return GreaterOf(X, Y); }

// Work over threads: Do(Item, Worker) for every Item from 0 to Count - 1, taken Run at a time by whichever of Workers
// threads is free - the caller's own (Worker 0) among them -, Worker a thread's number, for scratch room of its own. One
// item's work must not write what another's reads, nor depend on which went first: then what comes out is the same, to
// the bit, on any number of threads. A helper takes the caller's floating-point state (the SSE control word, the x87
// one) before it starts, so it rounds as the caller does; one that will not start leaves its share to the others.
static const int WorkersMost = 8;
template <typename F> inline void OnThreads(int Count, int Run, int Workers, const F& Do)
{
    if (Workers > WorkersMost)
        Workers = WorkersMost;
    if (Run < 1)
        Run = 1;
    if (Workers <= 1 || Count <= Run)
    {
        for (int i = 0; i < Count; i++)
            Do(i, 0);
        return;
    }
    const unsigned int Csr = _mm_getcsr();
#if defined(_M_IX86)
    unsigned int X87 = 0, Sse = 0;
    __control87_2(0, 0, &X87, &Sse);
#endif
    std::atomic<int> Next(0);
    auto Work = [&](int Worker)
    {
        for (;;)
        {
            const int From = Next.fetch_add(Run);
            if (From >= Count)
                return;
            const int To = (From < Count - Run) ? From + Run : Count;
            for (int i = From; i < To; i++)
                Do(i, Worker);
        }
    };
    std::thread Helper[WorkersMost];
    int         Started = 0;
    for (int w = 1; w < Workers; w++)
    {
        try
        {
            Helper[Started] = std::thread([&, w]()
            {
                _mm_setcsr(Csr);
#if defined(_M_IX86)
                unsigned int Was = 0;
                __control87_2(X87, _MCW_EM | _MCW_IC | _MCW_RC | _MCW_PC, &Was, nullptr);
#endif
                Work(w);
            });
            Started++;
        }
        catch (...)
        {
            break;
        }
    }
    Work(0);
    for (int k = 0; k < Started; k++)
        Helper[k].join();
}

// How many threads the work of one rounding may take where it may take them all: the processor's, less one left for the
// rest of the machine, at most WorkersMost.
inline int WorkersAvailable()
{
    const unsigned int N = std::thread::hardware_concurrency();
    return (N <= 2) ? 1 : (N - 1 > (unsigned int)WorkersMost) ? WorkersMost : (int)(N - 1);
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
    // The room made Want exactly - a list filled once to a length known ahead, which doubling its way there would leave
    // with up to twice that.
    bool ReserveExact(int Want)
    {
        if (Want <= Room)
            return true;
        T* const Grown = (T*)realloc(Data, (size_t)Want * sizeof(T));
        if (!Grown)
            return false;
        Data = Grown;
        Room = Want;
        return true;
    }
    // The list's block handed over and the list left empty: whoever takes it frees it (free).
    T* Release()
    {
        T* const Held = Data;
        Data          = nullptr;
        Num = Room = 0;
        return Held;
    }
    bool Add(const T& V)
    {
        if (Num == Room && !Reserve(Num + 1))
            return false;
        Data[Num++] = V;
        return true;
    }
    bool Resize(int N)
    {
        if (!Reserve(N))
            return false;
        Num = N;
        return true;
    }
    void Clear() { Num = 0; }
    // The room cut down to what is held: a list kept for long gives back what its growing left over.
    void Shrink()
    {
        if (Num == Room)
            return;
        if (Num == 0)
        {
            Free();
            return;
        }
        T* const Cut = (T*)realloc(Data, (size_t)Num * sizeof(T));
        if (Cut)
        {
            Data = Cut;
            Room = Num;
        }
    }
    void Free()
    {
        free(Data);
        Data = nullptr;
        Num = Room = 0;
    }
    T&       operator[](int i) { return Data[i]; }
    const T& operator[](int i) const { return Data[i]; }
};

// ---------------------------------------------------------------------------------------------------------------
// What goes in and what comes out
// ---------------------------------------------------------------------------------------------------------------

// What a polygon may take part in: a wall that may be rounded; and of those, a sheet drawn from both of its sides (glass,
// a net), which goes along with a wall it lies across (MeshRound.h's FShape::Make, Sheet); and a wall the relief may be
// drawn on - its patches laid by the card on the rounded surface itself, so the edges between it and the flat polygons
// beside it are cut close to their curve all along (LevelRound.h): what is drawn of the pieces there, under it or beside
// it, stands off that surface by no more than Sag.
enum
{
    Use_Wall   = 1,
    Use_Sheet  = 2,
    Use_Relief = 4
};

// What a polygon of the level takes part in, by its surface's flags (the engine's PF_ values) and whether a mover
// carries it: what is not drawn (invisible, a zone's portal, the sky's backdrop) is not rounded; nor is a mirror,
// whose view the engine sets up by its flat plane; nor a mover's polygon, which is not where the level left it.
inline int UseFor(unsigned int PolyFlags, bool bMover)
{
    const unsigned int Invisible = 0x00000001u, FakeBackdrop = 0x00000080u, TwoSided = 0x00000100u,
                       Portal = 0x04000000u, Mirrored = 0x08000000u;
    if (bMover || (PolyFlags & (Invisible | FakeBackdrop | Portal | Mirrored)))
        return 0;
    return Use_Wall | ((PolyFlags & TwoSided) ? Use_Sheet : 0);
}

// A polygon of the level: its corners in the input list, in order round it, and its plane - the normal of length one.
struct FPolyIn
{
    int    First, Count;
    double Nx, Ny, Nz, W;
    int    Use;
};

// A side of a polygon's own along which another polygon of its smooth surface lies across soft edges - for the light
// maps' blending over them (the renderer's SphereLight*). A, B: the side's ends; Into: the way into the polygon from
// the side, in its plane, of length one.
struct FLightSide
{
    FV3 A, B, Into;
    int PartFirst, PartCount;
};
// A run of a side across which one polygon lies: from T0 to T1 along the side from its end A (units); that polygon, the
// way into it from the side in its plane (of length one) and how far it reaches that way; and how far into the side's
// own polygon the light across is taken in.
struct FLightPart
{
    double T0, T1;
    int    Other;
    FV3    Into;
    double Depth, Reach;
};

struct FResult
{
    // Every place, welded: where it was, and where it is now.
    TList<FV3> Orig, Moved;
    // Per input polygon: its first triangle and how many (none: it is drawn as it was).
    TList<int> PolyFirst, PolyTris;
    // Three places a triangle, in the order round it the polygon has; and for each triangle how far the surface has
    // turned round the upright there, radians - what the relief's axes turn by (one a triangle: its corners share it).
    TList<int>   Tri;
    TList<float> TriTurn;
    // Each moved polygon's faces of the rounded surface - its flat polygon as one piece (LevelRound.h's FCoarse): the
    // quad it is, else its triangles that overlap the polygon -, each face's kind (0 a triangle, 1 + the axis its plane
    // is laid flat along for a quad), its four corners (a triangle's last its third) and the terms of its edges from
    // corner k to k + 1 (MeshRound.h's FEdgeTerms, LevelRound.h's FaceTermFloats floats a face) - for a point laid on
    // one (a decal, the laser sight's spot, the relief's patches) to be moved as the polygon is (LevelRound.h's
    // MoveOnPolygon), the cap they are held to, and those polygons by a grid over the plan (PolygonAt).
    TList<int>           PolyFaceFirst, PolyFaceCount;
    // Per input polygon, whether the relief may be drawn on it (Use_Relief): the card's copy keeps such a moved
    // polygon's triangles, for the relief to lie on them (LevelRound.h's PackRound).
    TList<unsigned char> PolyRelief;
    // And each one's plane as it came in (normal and distance): the floor's polygons lie in it - a thin one's triangles,
    // a corner of a neighbour's taken in a fraction of a unit off it, would tilt it by degrees.
    TList<float> PolyPlane;
    TList<unsigned char> FaceKind;
    TList<FV3>           FaceCorner;
    TList<float> FaceTerm;
    double       MoveCap    = 0.0;
    double       PolyGridX0 = 0.0, PolyGridY0 = 0.0;
    int          PolyGridW = 0, PolyGridH = 0;
    TList<int>   PolyGridFirst, PolyGridCount, PolyGridItems;
    // The polygons left to the engine: too many corners, or none to cut into triangles.
    TList<int> Left;
    // Per input polygon, how many of its triangles' edges that are no edge between two of its own triangles are of each
    // kind (MeshRound.h's FShape::Edge_Fixed .. Edge_Open, five a polygon) - what the measurements read.
    TList<int> PolyEdgeKinds;
    // Per input polygon its sides along soft edges (FLightSide), and their runs.
    TList<int>        PolySideFirst, PolySideCount;
    TList<FLightSide> LightSides;
    TList<FLightPart> LightParts;
    // The outlines of the moved walls the relief may be drawn on, as their triangles draw them (LevelRound.h's ChordsOf):
    // per input polygon the record of the edge from its first corner, -1 none - the edge from corner k to k + 1 is that
    // record plus k; per record its first point and how many, and the edge's two ends (two a record); per point its place
    // along the edge, 0 at the first end and 1 at the other, and how far it moved. The card moves the relief's outline
    // between those points in straight lines, as the pieces beside and under it are drawn (DSRelief).
    TList<int>   PolyChordFirst;
    TList<int>   ChordFirst, ChordCount;
    TList<FV3>   ChordEnds;
    TList<float> ChordT;
    TList<FV3>   ChordMove;
    // Of those walls, how many have their records, and how many have none: their triangles' sides made no ring.
    int ChordPolys = 0, ChordMissed = 0;
    // What it found: the polygons taking part, the triangles they were cut into, the T-junctions put in, the polygons
    // left to the engine (too many corners, or none to cut); the places taken out of the flat polygons as one piece and
    // those that could not be, the pieces, their soft edges and creases, the flat polygons rounded as quads (a Coons
    // patch each), the edges whose bends were halved for a piece not to fold over (Unbent, every time), the faces whose
    // sides' points were zipped together for their grid turned some of them over (Zipped); the polygons
    // moved and the most a place moved.
    int    Walls = 0, Segments = 0, SoftEdges = 0, Creases = 0, TPoints = 0, Folded = 0, Touched = 0;
    int    Through = 0, Stuck = 0, Pieces = 0, Quads = 0, Unbent = 0, Zipped = 0;
    double LargestMove = 0.0;
    // The places held back before a still polygon of the level (LevelRound.h's HoldBehind): those whose move would pass
    // through one, and those about them that give back part of theirs; which places those are (PlaceHeld, one a place -
    // empty where none was held), and per input polygon whether a corner of its triangles is one (PolyHeld): what is laid
    // on such a polygon goes as its drawn triangles go there (LevelRound.h's HoldOnPolygon).
    int                  HeldStopped = 0, HeldEased = 0;
    TList<unsigned char> PlaceHeld, PolyHeld;
    // How long each part took, milliseconds (LevelRound.h's steps): the welding (1), the T-junctions (2), the polygons
    // cut into triangles (3), the shape of them joined into one (4), the flat polygons as one piece each and the
    // surface of their shape (5), all of that made once more where a sheet is held (5 again), the moves - the edges'
    // and the triangles' cuts (6) -, the polygons (7), and the relief's outlines on the pieces' sides (ChordsOf) - by
    // the caller's clock (Seconds, none: not measured; the engine's headers make a macro of the C library's own).
    static const int Parts = 9;
    double PartMs[Parts] = {};
    double (*Seconds)() = nullptr;
    // How many threads the rounding may take (OnThreads): one, the caller's alone - beside a game that is being played -,
    // or more where it is waited for; what it makes is the same either way.
    int Workers = 1;

    // The memory its lists take, their room included; and all of it let go (a level left: the renderer's
    // SphereLevelsForget).
    template <class T> static size_t BytesOf(const TList<T>& L) { return (size_t)L.Room * sizeof(T); }
    size_t Bytes() const
    {
        return BytesOf(Orig) + BytesOf(Moved) + BytesOf(PolyFirst) + BytesOf(PolyTris) + BytesOf(Tri) +
               BytesOf(TriTurn) + BytesOf(PolyFaceFirst) + BytesOf(PolyFaceCount) + BytesOf(FaceKind) +
               BytesOf(FaceCorner) + BytesOf(FaceTerm) + BytesOf(PolyGridFirst) + BytesOf(PolyGridCount) +
               BytesOf(PolyGridItems) + BytesOf(Left) + BytesOf(PolyEdgeKinds) + BytesOf(PolySideFirst) +
               BytesOf(PolySideCount) + BytesOf(LightSides) + BytesOf(LightParts) + BytesOf(PolyChordFirst) +
               BytesOf(ChordFirst) + BytesOf(ChordCount) + BytesOf(ChordEnds) + BytesOf(ChordT) + BytesOf(ChordMove) +
               BytesOf(PlaceHeld) + BytesOf(PolyHeld);
    }
    void Free()
    {
        Orig.Free();
        Moved.Free();
        PolyFirst.Free();
        PolyTris.Free();
        Tri.Free();
        TriTurn.Free();
        PolyFaceFirst.Free();
        PolyFaceCount.Free();
        FaceKind.Free();
        FaceCorner.Free();
        FaceTerm.Free();
        PolyGridFirst.Free();
        PolyGridCount.Free();
        PolyGridItems.Free();
        PolyGridW = PolyGridH = 0;
        Left.Free();
        PolyEdgeKinds.Free();
        PolySideFirst.Free();
        PolySideCount.Free();
        LightSides.Free();
        LightParts.Free();
        PolyChordFirst.Free();
        ChordFirst.Free();
        ChordCount.Free();
        ChordEnds.Free();
        ChordT.Free();
        ChordMove.Free();
        PlaceHeld.Free();
        PolyHeld.Free();
    }
};

} // namespace RdxSphere

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
#include <process.h>
#include <xmmintrin.h>
// The helpers' threads are waited for and let go by their handles (OnThreads). Where nothing has brought Windows' header
// in yet (the unit tests), the lean part of it, without its min and max and without the drawing part's names.
#ifndef _WINDOWS_
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#endif
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

// A place in single precision: as the renderer draws it, and as a made level's places are kept (FResult::Compact).
struct FV3f
{
    float X, Y, Z;
};

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
// one) before it starts, so it rounds as the caller does, and runs at the caller's priority - a job beside the game
// below the game's threads keeps its helpers there too; one that will not start leaves its share to the others.
// The helpers are the C library's threads, started with their numbers in the caller's frame, which outlives them: no
// std::thread, which takes its callable from the global operator new and frees it on the thread it ran - in the game
// the engine's allocator, which is not thread-safe: helpers ending together would corrupt its lists.
static const int WorkersMost = 8;
template <typename W> struct FWorkerStart
{
    const W*     Work   = nullptr;
    int          Worker = 0;
    unsigned int Csr    = 0;
    unsigned int X87    = 0;
    static unsigned int __stdcall Main(void* Arg)
    {
        const FWorkerStart& S = *(const FWorkerStart*)Arg;
        _mm_setcsr(S.Csr);
#if defined(_M_IX86)
        unsigned int Was = 0;
        __control87_2(S.X87, _MCW_EM | _MCW_IC | _MCW_RC | _MCW_PC, &Was, nullptr);
#endif
        (*S.Work)(S.Worker);
        return 0;
    }
};
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
    unsigned int       X87 = 0;
#if defined(_M_IX86)
    unsigned int Sse = 0;
    __control87_2(0, 0, &X87, &Sse);
#endif
    const int        Priority = ::GetThreadPriority(::GetCurrentThread());
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
    typedef FWorkerStart<decltype(Work)> FStart;
    FStart Start[WorkersMost];
    HANDLE Helper[WorkersMost];
    int    Started = 0;
    for (int w = 1; w < Workers; w++)
    {
        FStart& S = Start[Started];
        S.Work    = &Work;
        S.Worker  = w;
        S.Csr     = Csr;
        S.X87     = X87;
        const uintptr_t H = _beginthreadex(nullptr, 0, &FStart::Main, &S, CREATE_SUSPENDED, nullptr);
        if (!H)
            break;
        ::SetThreadPriority((HANDLE)H, Priority);
        ::ResumeThread((HANDLE)H);
        Helper[Started++] = (HANDLE)H;
    }
    Work(0);
    if (Started > 0)
        ::WaitForMultipleObjects((DWORD)Started, Helper, TRUE, INFINITE);
    for (int k = 0; k < Started; k++)
        ::CloseHandle(Helper[k]);
}

// How many threads the work of one rounding may take where it may take them all: the processor's, less one left for the
// rest of the machine, at most WorkersMost.
inline int WorkersAvailable()
{
    const unsigned int N = std::thread::hardware_concurrency();
    return (N <= 2) ? 1 : (N - 1 > (unsigned int)WorkersMost) ? WorkersMost : (int)(N - 1);
}

// Whether a list may take a block of Bytes, asked by its growing (TList) for a block of RoomAskFrom bytes or more: the
// lists that grow with a level are the ones that come to be that large, and the question is a call into the system.
// None (the measurements', the tests'): a block is the system's to give or not. The game's (RenderDevice.cpp's
// SphereRoomFor) says no where the block would leave the process too little of its address space - the list's growing
// then fails as it does where the system has no room left.
typedef bool (*FRoomFor)(size_t Bytes);
inline FRoomFor& RoomFor()
{
    static FRoomFor Asked = nullptr;
    return Asked;
}
static const size_t RoomAskFrom = (size_t)1 << 20;

// The bytes every list holds at once, for the log's memory line at each level's load: what stays of them from level to
// level is the lists' own.
inline std::atomic<long long>& ListBytesHeld()
{
    static std::atomic<long long> Held(0);
    return Held;
}

// A rounding no longer wanted while it is made (the level it was for gone, its job waited for to be thrown away): its
// makers look at this between their parts - never within one, where helpers may be at work - and give up there as they
// do with no memory. Set by whoever waits for such a job, put back as soon as it has ended.
inline std::atomic<bool>& Abandoned()
{
    static std::atomic<bool> Flag(false);
    return Flag;
}

// A growing list of plain data.
template <typename T>
struct TList
{
    T*  Data = nullptr;
    int Num  = 0;
    int Room = 0;

    TList() {}
    ~TList()
    {
        ListBytesHeld() -= (long long)Room * (long long)sizeof(T);
        free(Data);
    }
    TList(const TList&)            = delete;
    TList& operator=(const TList&) = delete;

    // Grown by half again each time, not twice: a list filled as it goes holds at the most half its length spare, where
    // doubling would hold as much as its length - the largest lists are made so, and held at once.
    bool Reserve(int Want)
    {
        if (Want <= Room)
            return true;
        long long More = Room ? Room : 64;
        // Half again - and one, where half of it is none: a list of room for one would stay one, the loop endless.
        while (More < Want)
            More += (More > 1) ? More / 2 : 1;
        if (More > 0x7FFFFFFF)
            More = Want;
        return RoomOf(More);
    }
    // The room made Want exactly - a list filled once to a length known ahead, which doubling its way there would leave
    // with up to twice that.
    bool ReserveExact(int Want)
    {
        if (Want <= Room)
            return true;
        return RoomOf(Want);
    }
    // The block made Count long: none where its bytes are more than a size_t counts - a 32-bit build's, a list of
    // millions of large items - or than RoomFor allows.
    bool RoomOf(long long Count)
    {
        if ((unsigned long long)Count > (unsigned long long)((size_t)-1 / sizeof(T)))
            return false;
        const size_t Bytes = (size_t)Count * sizeof(T);
        if (Bytes >= RoomAskFrom && RoomFor() && !RoomFor()(Bytes))
            return false;
        T* const Grown = (T*)realloc(Data, Bytes);
        if (!Grown)
            return false;
        ListBytesHeld() += (long long)Bytes - (long long)Room * (long long)sizeof(T);
        Data = Grown;
        Room = (int)Count;
        return true;
    }
    // The list's block handed over and the list left empty: whoever takes it frees it (free).
    T* Release()
    {
        ListBytesHeld() -= (long long)Room * (long long)sizeof(T);   // no list's any more
        T* const Held = Data;
        Data          = nullptr;
        Num = Room = 0;
        return Held;
    }
    // The two lists' blocks traded - a list made aside taking the place of the one in use, in one step.
    void SwapWith(TList& Other)
    {
        T* const  D = Data;
        const int N = Num, R = Room;
        Data        = Other.Data;
        Num         = Other.Num;
        Room        = Other.Room;
        Other.Data  = D;
        Other.Num   = N;
        Other.Room  = R;
    }
    bool Add(const T& V)
    {
        if (Num == Room && !Reserve(Num + 1))
            return false;
        Data[Num++] = V;
        return true;
    }
    // A list sized for the first time gets its room exactly: most are sized once to a length known ahead, and a
    // doubling's spare part would be held as long as they are.
    bool Resize(int N)
    {
        if (!(Room == 0 ? ReserveExact(N) : Reserve(N)))
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
            ListBytesHeld() -= (long long)(Room - Num) * (long long)sizeof(T);
            Data = Cut;
            Room = Num;
        }
    }
    void Free()
    {
        ListBytesHeld() -= (long long)Room * (long long)sizeof(T);
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
    // Every place, welded: where it was, and where it is now - in double precision while the level is made, and once it
    // is made in single (KeptOrig, KeptMoved; Compact), as the renderer draws them, for half the room through the level's
    // stay. Read by OrigAt and MovedAt, which take either, and counted by Places.
    TList<FV3>  Orig, Moved;
    TList<FV3f> KeptOrig, KeptMoved;
    int         Places() const { return (KeptOrig.Num > 0) ? KeptOrig.Num : Orig.Num; }
    FV3         OrigAt(int i) const
    {
        if (KeptOrig.Num == 0)
            return Orig.Data[i];
        const FV3f& P = KeptOrig.Data[i];
        return V3(P.X, P.Y, P.Z);
    }
    FV3 MovedAt(int i) const
    {
        if (KeptMoved.Num == 0)
            return Moved.Data[i];
        const FV3f& P = KeptMoved.Data[i];
        return V3(P.X, P.Y, P.Z);
    }
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
    // And for that lookup, made with the faces: each face's plane as FaceSharesAt makes it - the cross product of the
    // edges from its first corner to its second and to its last (a triangle's third, a quad's fourth), and that one's
    // length, four numbers a face - and each moved polygon's faces' corners' box, the lowest X, Y and Z and then the
    // highest, six numbers a polygon: a point that far outside it lies on none of its faces (MoveOnPolygon).
    TList<double> FacePlane, PolyBox;
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
    // Per input polygon its ring of places, the T-junctions put in (LevelRound.h's step 2): from RingFirst, RingCount of
    // them, in order round it; RingOwn 1 where the place is one of the polygon's own corners. Kept for the bevel
    // (LevelBevel.h), which cuts the still polygons along their hard edges, and let go after it.
    TList<int>           Ring, RingFirst, RingCount;
    TList<unsigned char> RingOwn;
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
    // Per place the fine bevel moved (LevelBevel.h), its round's outward normal there, a word - each of its numbers in
    // ten bits, as the card takes a normal (RenderDevice.cpp's PackNormal) -, and 0 for every other place: a place on
    // the round is shaded by the round's own normal, one beside it by its face's. Empty where no bevel was made.
    TList<unsigned int> BevelNormal;
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
    // How far a cut piece of a curve may stand off it at its middle, in the level's units (LevelRound.h's HighSag at the
    // spherifier's High): the spherifier's level (RenderMath.h's SpherifierSagOf).
    float Sag = 0.25f;
    // A measurement's alone (LevelRoundCheck's): units, the flat polygons' pieces each kept within one cube of this size
    // - a place taken out onto a neighbour in its own cube only, and no edge turned to join two cubes' places -, for how
    // the rounding would look made a cube at a time. None (0): each flat polygon one piece, as the renderer draws it.
    double PieceCube = 0.0;

    // The memory its lists take, their room included; and all of it let go (a level left: the renderer's
    // SphereLevelsForget).
    template <class T> static size_t BytesOf(const TList<T>& L) { return (size_t)L.Room * sizeof(T); }
    size_t Bytes() const
    {
        return BytesOf(Orig) + BytesOf(Moved) + BytesOf(KeptOrig) + BytesOf(KeptMoved) + BytesOf(PolyFirst) +
               BytesOf(PolyTris) + BytesOf(Tri) + BytesOf(PolyRelief) + BytesOf(PolyPlane) + BytesOf(Ring) +
               BytesOf(RingFirst) + BytesOf(RingCount) + BytesOf(RingOwn) +
               BytesOf(TriTurn) + BytesOf(PolyFaceFirst) + BytesOf(PolyFaceCount) + BytesOf(FaceKind) +
               BytesOf(FaceCorner) + BytesOf(FaceTerm) + BytesOf(FacePlane) + BytesOf(PolyBox) +
               BytesOf(PolyGridFirst) + BytesOf(PolyGridCount) +
               BytesOf(PolyGridItems) + BytesOf(Left) + BytesOf(PolyEdgeKinds) + BytesOf(PolySideFirst) +
               BytesOf(PolySideCount) + BytesOf(LightSides) + BytesOf(LightParts) + BytesOf(PolyChordFirst) +
               BytesOf(ChordFirst) + BytesOf(ChordCount) + BytesOf(ChordEnds) + BytesOf(ChordT) + BytesOf(ChordMove) +
               BytesOf(PlaceHeld) + BytesOf(PolyHeld) + BytesOf(BevelNormal);
    }
    void Free()
    {
        Orig.Free();
        Moved.Free();
        KeptOrig.Free();
        KeptMoved.Free();
        PolyRelief.Free();
        PolyPlane.Free();
        Ring.Free();
        RingFirst.Free();
        RingCount.Free();
        RingOwn.Free();
        PolyFirst.Free();
        PolyTris.Free();
        Tri.Free();
        TriTurn.Free();
        PolyFaceFirst.Free();
        PolyFaceCount.Free();
        FaceKind.Free();
        FaceCorner.Free();
        FaceTerm.Free();
        FacePlane.Free();
        PolyBox.Free();
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
        BevelNormal.Free();
    }
    // The places in single precision, the double ones let go, once nothing is made of them any more: what is drawn of
    // them is the same, the renderer taking them as FLOATs. False where there was no memory for it - the double ones are
    // kept then.
    bool Compact()
    {
        if (KeptOrig.Num > 0 || Orig.Num == 0 || Moved.Num != Orig.Num)
            return true;
        if (!KeptOrig.ReserveExact(Orig.Num) || !KeptMoved.ReserveExact(Moved.Num))
        {
            KeptOrig.Free();
            KeptMoved.Free();
            return false;
        }
        for (int i = 0; i < Orig.Num; i++)
        {
            const FV3 A = Orig.Data[i], B = Moved.Data[i];
            const FV3f Af = { (float)A.X, (float)A.Y, (float)A.Z }, Bf = { (float)B.X, (float)B.Y, (float)B.Z };
            KeptOrig.Data[i]  = Af;
            KeptMoved.Data[i] = Bf;
        }
        KeptOrig.Num = KeptMoved.Num = Orig.Num;
        Orig.Free();
        Moved.Free();
        return true;
    }
};

} // namespace RdxSphere

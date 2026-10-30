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
// In (little-endian): "SPI1", the cap (double), the places (int count, then three doubles each), the polygons (int
// count, then each one's first place, count, plane's four doubles and use). One line to the console: what was made,
// what failed, and how long it took.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

#include <dbghelp.h>
#include <mmsystem.h>
#include <algorithm>
#include <map>
#include <string>
#include <tuple>
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

int main(int Argc, char** Argv)
{
    if (Argc < 2)
    {
        printf("usage: LevelRoundCheck <in> [angle]\n");
        return 2;
    }
    const double Angle = (Argc > 2) ? atof(Argv[2]) : 105.0;
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

    if (const char* Box = getenv("LRC_TRACE_BOX"))
        sscanf(Box, "%lf,%lf,%lf,%lf,%lf,%lf", &GTraceBox[0], &GTraceBox[1], &GTraceBox[2], &GTraceBox[3], &GTraceBox[4],
               &GTraceBox[5]);
    FResult R;
    // LRC_WORKERS=<n>: the rounding on n threads (one: the caller's alone); none, as many as it may take where it is
    // waited for (RdxSphere::WorkersAvailable). What it makes must not change with it.
    R.Workers = RdxSphere::WorkersAvailable();
    if (const char* Workers = getenv("LRC_WORKERS"))
        R.Workers = (atoi(Workers) > 0) ? atoi(Workers) : 1;
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
    const double T0    = Now();
    R.Seconds          = Now;
    const bool   bMade = RdxLevelRound::Round(Pts.Data, NumPts, Polys.Data, NumPolys, Cap, Angle, R);
    const double Ms    = (Now() - T0) * 1000.0;
    if (!bMade)
    {
        printf("no memory\n");
        return 1;
    }
    printf("result hash: %016llx (on %d threads)\n", ResultHash(R), R.Workers);

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
    int          Turned = 0, TurnedInPlane = 0, TurnedSliver = 0, Flat = 0;
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
            if (bDetail && Said < 40)
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
                       "the triangle was %.3f square units, is %.3f, its corners moved %.2f %.2f %.2f\n",
                       p, P.Count, X1 - X0, Y1 - Y0, Z1 - Z0, P.Nx, P.Ny, P.Nz, R.Orig[A].X, R.Orig[A].Y, R.Orig[A].Z,
                       0.5 * RdxSphere::Length(Was), 0.5 * L, RdxSphere::Length(R.Moved[A] - R.Orig[A]),
                       RdxSphere::Length(R.Moved[B] - R.Orig[B]), RdxSphere::Length(R.Moved[C] - R.Orig[C]));
            }
        }
    const int TurnedShowing = Turned - TurnedInPlane - TurnedSliver;
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
    return (Over || Open || TurnedShowing || BadSides) ? 1 : 0;
}

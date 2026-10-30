// MeshRoundCheck: the renderer's model rounding (src\RevisionDX12\MeshRound.h) run on a model the game drew, as the
// renderer wrote it down (the marker SecondRevision-mesh-dump.txt: SecondRevision-mesh-round.bin, each model as it was
// first known - its faces, which are drawn, its vertices' places in the view and the engine's normals there). No game and
// no graphics card: what the shape makes of the model is said, and its rounded surface written out to be looked at.
//
//    MeshRoundCheck <dump.bin> <model name> <out.obj> [angle, degrees] [cap, units] [cuts]
//
// The OBJ holds the model twice: as the engine draws it (group "flat") and rounded (group "round"), every face cut into
// cuts x cuts pieces and moved as the domain shader moves them (MeshRound.h's PatchMoveAt - along the plane of a flat
// polygon of several faces its PolyPlaneMoveAt -, held to the cap).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "MeshRound.h"

using namespace RdxMeshRound;

struct FDumped
{
    char             Name[65];
    int              Verts = 0, Faces = 0;
    std::vector<int> Corner;
    std::vector<unsigned char> Round;
    std::vector<float>         Sample;   // 6 a vertex: place, then the engine's normal
};

static bool ReadDump(const char* Path, std::vector<FDumped>& Out)
{
    FILE* F = nullptr;
    if (fopen_s(&F, Path, "rb") != 0 || !F)
        return false;
    for (;;)
    {
        int Head[3];
        if (fread(Head, sizeof(int), 3, F) != 3)
            break;
        FDumped M;
        M.Verts = Head[1];
        M.Faces = Head[2];
        if (fread(M.Name, 1, 64, F) != 64)
            break;
        M.Name[64] = 0;
        M.Corner.resize((size_t)M.Faces * 3);
        M.Round.resize((size_t)M.Faces);
        M.Sample.resize((size_t)M.Verts * 6);
        if (fread(M.Corner.data(), sizeof(int), M.Corner.size(), F) != M.Corner.size() ||
            fread(M.Round.data(), 1, M.Round.size(), F) != M.Round.size() ||
            fread(M.Sample.data(), sizeof(float), M.Sample.size(), F) != M.Sample.size())
            break;
        Out.push_back(M);
    }
    fclose(F);
    return true;
}

int main(int Argc, char** Argv)
{
    if (Argc < 4)
    {
        printf("MeshRoundCheck <dump.bin> <model name> <out.obj> [angle] [cap] [cuts]\n");
        return 1;
    }
    std::vector<FDumped> Models;
    if (!ReadDump(Argv[1], Models))
    {
        printf("cannot read %s\n", Argv[1]);
        return 1;
    }
    const float Angle = (Argc > 4) ? (float)atof(Argv[4]) : 105.0f;
    const float Cap   = (Argc > 5) ? (float)atof(Argv[5]) : 8.0f;
    const int   Cuts  = (Argc > 6) ? atoi(Argv[6]) : 8;
    const FDumped* D = nullptr;
    for (const FDumped& M : Models)
        if (_stricmp(M.Name, Argv[2]) == 0)
            D = &M;
    if (!D)
    {
        printf("no model %s in the dump; it holds:\n", Argv[2]);
        for (const FDumped& M : Models)
            printf("  %s (%i vertices, %i faces)\n", M.Name, M.Verts, M.Faces);
        return 1;
    }
    FModel Model;
    if (!Model.Make(D->Corner.data(), D->Faces, D->Verts))
    {
        printf("the model's faces make no model\n");
        return 1;
    }
    FPose Pose;
    Pose.Base   = (const char*)D->Sample.data();
    Pose.Stride = 6 * (int)sizeof(float);
    static FShape Shape;
    if (!Shape.Make(Model, D->Round.data(), Pose, 180.0f - Angle, Cap))
    {
        printf("the model makes no shape\n");
        return 1;
    }
    Shape.Pose(Pose, 1);
    int Kinds[5] = { 0, 0, 0, 0, 0 };
    for (int E = 0; E < Shape.Edges; E++)
        Kinds[Shape.EdgeKind[E]]++;
    int Welded = 0;
    for (int v = 0; v < D->Verts; v++)
        Welded += (Shape.Weld[v] == v) ? 1 : 0;
    printf("%s: %i vertices (%i places), %i faces, %i fans; edges: %i fixed, %i inner, %i soft, %i creases, %i open "
           "and free\n", D->Name, D->Verts, Welded, D->Faces, Shape.Fans, Kinds[0], Kinds[1], Kinds[2], Kinds[3],
           Kinds[4]);
    // The edges that bend most, and those whose bends are kept nearest their corners.
    float Most = 0.0f;
    int   Short = 0;
    for (int E = 0; E < Shape.Edges; E++)
    {
        FEdgeTerms T;
        Shape.EdgeData(E, T);
        for (int k = 0; k < EDGE_SLOTS; k++)
        {
            const float L = Length(T.T[k]) * T.Room[k] * BendTop;
            Most          = (L > Most) ? L : Most;
            Short += (Length(T.T[k]) > 1e-3f && T.Room[k] < 0.5f) ? 1 : 0;
        }
    }
    printf("the most an edge's bend moves it: %.3f units; bends kept within less than half their edge: %i\n", Most, Short);
    // The fans' normals - the shape's own, its faces' weighed by their angles at the vertex - against the faces' areas'
    // sum, which a sliver of a face hardly moves: how many turn off it by more than ten degrees, and the most.
    {
        int   Off10 = 0, Worst = -1;
        float MostOff = 0.0f;
        for (int F = 0; F < Shape.Fans; F++)
        {
            const FV N    = Shape.FanNormal(F);
            FV       Area = { 0.0f, 0.0f, 0.0f };
            for (int i = Shape.FanStart[F]; i < Shape.FanStart[F + 1]; i++)
            {
                const int f = Shape.FanFaces[i];
                const FV  A = Pose.At(Model.Corner[f * 3]), B = Pose.At(Model.Corner[f * 3 + 1]);
                const FV  C = Pose.At(Model.Corner[f * 3 + 2]);
                Area        = Area + RdxMeshRound::Cross(B - A, C - A);
            }
            const float L = RdxMeshRound::Length(Area), Ln = RdxMeshRound::Length(N);
            if (L < 1e-12f || Ln < 1e-6f)
                continue;
            const float Cos = RdxMeshRound::Dot(N, Area) / (L * Ln);
            const float Deg = acosf(Cos < -1.0f ? -1.0f : (Cos > 1.0f ? 1.0f : Cos)) * 57.29578f;
            Off10 += (Deg > 10.0f) ? 1 : 0;
            if (Deg > MostOff)
            {
                MostOff = Deg;
                Worst   = F;
            }
        }
        printf("fans whose normal turns off their faces' areas' by more than ten degrees: %i of %i, the most %.1f "
               "degrees (fan %i)\n", Off10, Shape.Fans, MostOff, Worst);
    }
    // With "open" for the seventh word, every edge that has one face only: where it lies, and whether it is free.
    if (Argc > 7 && _stricmp(Argv[7], "open") == 0)
    {
        for (int E = 0; E < Shape.Edges; E++)
        {
            if (Shape.EdgeCount[E] != 1)
                continue;
            const FV A = Pose.At(Shape.EdgeA[E]), B = Pose.At(Shape.EdgeB[E]);
            printf("open edge %4i %s: (%7.2f %7.2f %7.2f) - (%7.2f %7.2f %7.2f), face %i\n", E,
                   (Shape.EdgeKind[E] == FShape::Edge_Open) ? "free " : "fixed", A.X, A.Y, A.Z, B.X, B.Y, B.Z,
                   Shape.EdgeC0[E] / 3);
        }
    }
    // With a number for the seventh word, every edge that bends by more than that many units: its ends, kind, length,
    // and each bend with its room - and the edge said to carry it on past each end.
    else if (Argc > 7)
    {
        const float Show = (float)atof(Argv[7]);
        for (int E = 0; E < Shape.Edges; E++)
        {
            FEdgeTerms T;
            Shape.EdgeData(E, T);
            float Bend = 0.0f;
            for (int k = 0; k < EDGE_SLOTS; k++)
                Bend += Length(T.T[k]) * T.Room[k] * BendTop;
            if (Bend < Show)
                continue;
            const FV    A = Pose.At(Shape.EdgeA[E]), B = Pose.At(Shape.EdgeB[E]);
            const float L = Length(B - A);
            printf("edge %4i kind %i: %4i (%7.2f %7.2f %7.2f) - %4i (%7.2f %7.2f %7.2f), %6.2f long; on past A: %i, B: %i\n",
                   E, Shape.EdgeKind[E], Shape.EdgeA[E], A.X, A.Y, A.Z, Shape.EdgeB[E], B.X, B.Y, B.Z, L,
                   Shape.OnEdge[E * 2], Shape.OnEdge[E * 2 + 1]);
            for (int k = 0; k < EDGE_SLOTS; k++)
                if (Length(T.T[k]) > 1e-4f)
                    printf("     bend %i: (%6.2f %6.2f %6.2f) %6.2f long, room %.3f\n", k, T.T[k].X, T.T[k].Y, T.T[k].Z,
                           Length(T.T[k]), T.Room[k]);
        }
    }

    FILE* O = nullptr;
    if (fopen_s(&O, Argv[3], "w") != 0 || !O)
    {
        printf("cannot write %s\n", Argv[3]);
        return 1;
    }
    for (int v = 0; v < D->Verts; v++)
        fprintf(O, "v %g %g %g\n", D->Sample[v * 6], D->Sample[v * 6 + 1], D->Sample[v * 6 + 2]);
    fprintf(O, "g flat\n");
    for (int f = 0; f < D->Faces; f++)
        if (D->Round[f])
            fprintf(O, "f %i %i %i\n", D->Corner[f * 3] + 1, D->Corner[f * 3 + 1] + 1, D->Corner[f * 3 + 2] + 1);
    // The rounded faces: a grid of points over each, cut as the patches are, each moved and held to the cap.
    int Next = D->Verts + 1;
    fprintf(O, "g round\n");
    float Moved = 0.0f;
    for (int f = 0; f < D->Faces; f++)
    {
        if (!D->Round[f])
            continue;
        const FEdgeTerms E[3] = { Shape.EdgeTerms(f, 0), Shape.EdgeTerms(f, 1), Shape.EdgeTerms(f, 2) };
        const FV P0 = Pose.At(D->Corner[f * 3]), P1 = Pose.At(D->Corner[f * 3 + 1]), P2 = Pose.At(D->Corner[f * 3 + 2]);
        // Its flat polygon's outline, where it is one of several faces.
        int        Outline[POLY_EDGES_MOST];
        FEdgeTerms OutTerms[POLY_EDGES_MOST];
        FV         OutEnds[POLY_EDGES_MOST * 2];
        const int  OutNum = Shape.PolyOutline(f, Outline, POLY_EDGES_MOST);
        for (int g = 0; g < OutNum; g++)
        {
            Shape.EdgeData(Outline[g], OutTerms[g]);
            OutEnds[g * 2]     = Pose.At(Shape.EdgeA[Outline[g]]);
            OutEnds[g * 2 + 1] = Pose.At(Shape.EdgeB[Outline[g]]);
        }
        const FV Plane = Shape.PlaneOfFace(Shape.Poly[f]);
        std::vector<int> Index((size_t)(Cuts + 1) * (Cuts + 1), 0);
        for (int i = 0; i <= Cuts; i++)
            for (int j = 0; j <= Cuts - i; j++)
            {
                const float B1 = (float)i / (float)Cuts, B2 = (float)j / (float)Cuts, B0 = 1.0f - B1 - B2;
                FV          Move = PatchMoveAt(E, B0, B1, B2);
                if (OutNum > 0)
                    Move = PolyPlaneMoveAt(OutTerms, OutEnds, OutNum, Plane, P0 * B0 + P1 * B1 + P2 * B2) +
                           Plane * Dot(Move, Plane);
                const float L    = Length(Move);
                if (L > Cap)
                    Move = Move * (Cap / L);
                Moved        = (L > Moved) ? L : Moved;
                const FV Flat = P0 * B0 + P1 * B1 + P2 * B2;
                const FV Q    = Flat + Move;
                fprintf(O, "v %g %g %g\n", Q.X, Q.Y, Q.Z);
                // Where the point lay before it moved: what the texture there is, so a picture of the rounded surface
                // can be laid with it and show how far the texture is dragged.
                fprintf(O, "vt %g %g %g\n", Flat.X, Flat.Y, Flat.Z);
                Index[(size_t)i * (Cuts + 1) + j] = Next++;
            }
        for (int i = 0; i < Cuts; i++)
            for (int j = 0; j < Cuts - i; j++)
            {
                const int A = Index[(size_t)i * (Cuts + 1) + j], B = Index[(size_t)(i + 1) * (Cuts + 1) + j],
                          C = Index[(size_t)i * (Cuts + 1) + j + 1];
                fprintf(O, "f %i %i %i\n", A, B, C);
                if (j + 1 <= Cuts - i - 1)
                {
                    const int Dd = Index[(size_t)(i + 1) * (Cuts + 1) + j + 1];
                    fprintf(O, "f %i %i %i\n", B, Dd, C);
                }
            }
    }
    fclose(O);
    printf("the most a point moves: %.3f units; written %s\n", Moved, Argv[3]);
    return 0;
}

// TreeCheck: the trees and bushes the renderer grows (TreeGen.h), drawn with no game and no graphics card - each kind
// of the game's plants at its class's size, from two sides and its trunk from near, in the bark and leaves the renderer
// draws for them, lit by a low sun; a tree's trunk round a trunk as thick as the game's own models' on Liberty Island.
// What it prints is what a level's loading pays: each plant's nodes, leaves, triangles and growing time.
//
//     TreeCheck [-out <folder>] [-seeds <n>] [-own]
//
// -own: every kind the own way (Trees3D's OWN), as without the leaf cards' sheet.
// -level <name>: the seeds a level's plants grow from (the renderer's ReadLevelTrees: the level's name, its variants in
// turn), and Tree1 at Hell's Kitchen's size - 250 tall, 219 across, its crown from its first tenth, widest at the top.
//
// The pictures go to the folder as BMP files, one a plant (both sides in it), and a line a plant to the console.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "TreeGen.h"
#include "PomArchive.h"

using namespace RdxTree;

// ---------------------------------------------------------------------------------------------------------------
// A picture and its depth
// ---------------------------------------------------------------------------------------------------------------

struct FImage
{
    int    Width = 0, Height = 0;
    float* Colour = nullptr;    // R G B a pixel
    float* Depth  = nullptr;    // the view's distance; larger is further

    void Make(int W, int H)
    {
        Width  = W;
        Height = H;
        Colour = (float*)calloc((size_t)W * H * 3, sizeof(float));
        Depth  = (float*)malloc((size_t)W * H * sizeof(float));
        for (int i = 0; i < W * H; i++)
            Depth[i] = 1e30f;
    }
    ~FImage()
    {
        free(Colour);
        free(Depth);
    }
};

struct FTexture
{
    int            Width, Height;
    unsigned char* Rgba;
    bool           bWrap;
};

static void Sample(const FTexture& T, float U, float V, float* Out)
{
    if (T.bWrap)
    {
        U -= floorf(U);
        V -= floorf(V);
    }
    else
    {
        U = (U < 0.0f) ? 0.0f : ((U > 1.0f) ? 1.0f : U);
        V = (V < 0.0f) ? 0.0f : ((V > 1.0f) ? 1.0f : V);
    }
    const float X = U * (float)T.Width - 0.5f, Y = V * (float)T.Height - 0.5f;
    const int   X0 = (int)floorf(X), Y0 = (int)floorf(Y);
    const float Fx = X - (float)X0, Fy = Y - (float)Y0;
    float Sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++)
        {
            int Xi = X0 + i, Yj = Y0 + j;
            if (T.bWrap)
            {
                Xi = ((Xi % T.Width) + T.Width) % T.Width;
                Yj = ((Yj % T.Height) + T.Height) % T.Height;
            }
            else
            {
                Xi = (Xi < 0) ? 0 : ((Xi >= T.Width) ? T.Width - 1 : Xi);
                Yj = (Yj < 0) ? 0 : ((Yj >= T.Height) ? T.Height - 1 : Yj);
            }
            const float W = (i ? Fx : 1.0f - Fx) * (j ? Fy : 1.0f - Fy);
            const unsigned char* const P = T.Rgba + 4 * (Yj * T.Width + Xi);
            for (int c = 0; c < 4; c++)
                Sum[c] += W * (float)P[c] / 255.0f;
        }
    for (int c = 0; c < 4; c++)
        Out[c] = Sum[c];
}

// ---------------------------------------------------------------------------------------------------------------
// The camera and the triangles
// ---------------------------------------------------------------------------------------------------------------

struct FCamera
{
    FVec  Eye, Right, Up, Ahead;
    float Focal, Cx, Cy;

    // Into the picture: x right, y down, z the distance along the view.
    bool Project(FVec P, float& X, float& Y, float& Z) const
    {
        const FVec D = P - Eye;
        Z = Dot(D, Ahead);
        if (Z < 1.0f)
            return false;
        X = Cx + Focal * Dot(D, Right) / Z;
        Y = Cy - Focal * Dot(D, Up) / Z;
        return true;
    }
};

struct FCorner
{
    float X, Y, Z;      // on the picture, and the distance
    float U, V;
    float Light;        // the light already worked out at the corner
};

static float Edge(const FCorner& A, const FCorner& B, float X, float Y)
{
    return (B.X - A.X) * (Y - A.Y) - (B.Y - A.Y) * (X - A.X);
}

// One triangle: the texture's colour times the light, kept where it is nearer than what is there - and where the
// texture's alpha is over a half, the masked test the game's own drawing makes.
// -why: the bark triangle each pixel of a picture was drawn by (-1 none), set by DrawMesh as it goes.
static int* GTriAt  = nullptr;
static int  GTriNow = -1;

static void DrawTriangle(FImage& Img, const FCorner& A, const FCorner& B, const FCorner& C, const FTexture& T)
{
    const float Area = Edge(A, B, C.X, C.Y);
    if (fabsf(Area) < 1e-8f)
        return;
    int X0 = (int)floorf(fminf(A.X, fminf(B.X, C.X))), X1 = (int)ceilf(fmaxf(A.X, fmaxf(B.X, C.X)));
    int Y0 = (int)floorf(fminf(A.Y, fminf(B.Y, C.Y))), Y1 = (int)ceilf(fmaxf(A.Y, fmaxf(B.Y, C.Y)));
    X0 = (X0 < 0) ? 0 : X0;
    Y0 = (Y0 < 0) ? 0 : Y0;
    X1 = (X1 > Img.Width - 1) ? Img.Width - 1 : X1;
    Y1 = (Y1 > Img.Height - 1) ? Img.Height - 1 : Y1;
    const float Ia = 1.0f / A.Z, Ib = 1.0f / B.Z, Ic = 1.0f / C.Z;
    for (int y = Y0; y <= Y1; y++)
        for (int x = X0; x <= X1; x++)
        {
            const float Px = (float)x + 0.5f, Py = (float)y + 0.5f;
            float Wa = Edge(B, C, Px, Py) / Area, Wb = Edge(C, A, Px, Py) / Area, Wc = Edge(A, B, Px, Py) / Area;
            if (Wa < 0.0f || Wb < 0.0f || Wc < 0.0f)
                continue;
            // Correct for the perspective: weights over the distance.
            const float Inv = Wa * Ia + Wb * Ib + Wc * Ic;
            const float Z   = 1.0f / Inv;
            const int   At  = y * Img.Width + x;
            if (Z >= Img.Depth[At])
                continue;
            const float Ka = Wa * Ia * Z, Kb = Wb * Ib * Z, Kc = Wc * Ic * Z;
            const float U = A.U * Ka + B.U * Kb + C.U * Kc, V = A.V * Ka + B.V * Kb + C.V * Kc;
            const float L = A.Light * Ka + B.Light * Kb + C.Light * Kc;
            float Tex[4];
            Sample(T, U, V, Tex);
            if (Tex[3] < 0.5f)
                continue;
            Img.Depth[At] = Z;
            if (GTriAt)
                GTriAt[At] = GTriNow;
            for (int c = 0; c < 3; c++)
                Img.Colour[3 * At + c] = Tex[c] * L;
        }
}

// ---------------------------------------------------------------------------------------------------------------
// A plant, drawn
// ---------------------------------------------------------------------------------------------------------------

static const FVec GSun = { -0.48f, -0.36f, 0.80f };

static void Background(FImage& Img, const FCamera& Cam)
{
    for (int y = 0; y < Img.Height; y++)
        for (int x = 0; x < Img.Width; x++)
        {
            // The ray through the pixel: down to the ground, or up into the sky.
            const FVec Dir = Unit(Cam.Ahead * Cam.Focal + Cam.Right * ((float)x + 0.5f - Cam.Cx) -
                                      Cam.Up * ((float)y + 0.5f - Cam.Cy),
                                  Cam.Ahead);
            float* const C = Img.Colour + 3 * (y * Img.Width + x);
            if (Dir.Z < -1e-4f)
            {
                const float T  = -Cam.Eye.Z / Dir.Z;
                const FVec  G  = Cam.Eye + Dir * T;
                const float Ck = (((int)floorf(G.X / 32.0f) + (int)floorf(G.Y / 32.0f)) & 1) ? 1.0f : 0.92f;
                C[0] = 0.34f * Ck;
                C[1] = 0.40f * Ck;
                C[2] = 0.27f * Ck;
            }
            else
            {
                const float Up = Dir.Z;
                C[0] = 0.62f + 0.18f * (1.0f - Up);
                C[1] = 0.72f + 0.14f * (1.0f - Up);
                C[2] = 0.86f + 0.08f * (1.0f - Up);
            }
        }
}

// The bark's inside where the view sees it - into a tube left open, or through a gap between two - in a red no bark or
// leaf has, so that a hole shows in the picture and can be counted (HolePixels). The game draws the bark from both
// sides, and a hole there shows the wood's inside, or the sky through it.
static unsigned char GHoleRgba[4] = { 255, 0, 0, 255 };
static const FTexture GHole = { 1, 1, GHoleRgba, true };

// Not within a few units of the eye: this drawing leaves out a triangle with a corner behind the eye, rather than cutting
// it, so the wood right at the eye shows its far side through its missing near one - which the game's drawing has not.
static const float HoleNearest = 24.0f;

static int HolePixels(const FImage& Img)
{
    int Count = 0;
    for (int i = 0; i < Img.Width * Img.Height; i++)
        Count += (Img.Colour[3 * i] > 0.99f && Img.Colour[3 * i + 1] < 0.001f && Img.Colour[3 * i + 2] < 0.001f &&
                  Img.Depth[i] > HoleNearest) ? 1 : 0;
    return Count;
}

static void DrawMesh(FImage& Img, const FCamera& Cam, const TList<FVertex>& Verts, const TList<unsigned int>& Index,
                     const FTexture& Tex, bool bLeaf)
{
    const FVec Sun = Unit(GSun, Vec(0.0f, 0.0f, 1.0f));
    for (int t = 0; t + 2 < Index.Num; t += 3)
    {
        FCorner C[3];
        bool    bIn = true;
        // A bark triangle whose outside faces away from the eye - well away, not a ridge of the bark's seen edgewise
        // at the wood's outline: its inside is what the view has of it (GHole).
        bool    bInside = false;
        if (!bLeaf)
        {
            const FVertex& A   = Verts[Index[t]];
            const FVertex& B   = Verts[Index[t + 1]];
            const FVertex& D   = Verts[Index[t + 2]];
            const FVec     Mid = Vec((A.X + B.X + D.X) / 3.0f, (A.Y + B.Y + D.Y) / 3.0f, (A.Z + B.Z + D.Z) / 3.0f);
            const FVec     Out = Vec(A.NX + B.NX + D.NX, A.NY + B.NY + D.NY, A.NZ + B.NZ + D.NZ);
            const FVec     To  = Cam.Eye - Mid;
            // Below the ground (a stem's foot sunk into it) the game's ground hides it; this picture's is only drawn
            // behind everything.
            bInside = Mid.Z > 0.5f && Dot(Out, To) < -0.3f * Length(Out) * Length(To);
        }
        for (int k = 0; k < 3 && bIn; k++)
        {
            const FVertex& V = Verts[Index[t + k]];
            bIn = Cam.Project(Vec(V.X, V.Y, V.Z), C[k].X, C[k].Y, C[k].Z);
            C[k].U = V.U;
            C[k].V = V.V;
            const float NL = V.NX * Sun.X + V.NY * Sun.Y + V.NZ * Sun.Z;
            // A leaf takes the sun on either face, and some through itself; the bark on its lit side alone. The sky's
            // light by how open the plant is there all round, the sun's by how open it is towards the sun (FOpenness).
            const float Lit   = bLeaf ? 0.55f + 0.45f * fabsf(NL) : ((NL > 0.0f) ? NL : 0.0f);
            const float Round = OpenGain * (float)V.Open[0] / 255.0f;
            const float Sky   = OpenFloor + (1.0f - OpenFloor) * ((Round < 1.0f) ? Round : 1.0f);
            const float ToSun = OpenFloor + (1.0f - OpenFloor) * OpenTowards(V.Open, Sun);
            C[k].Light = 0.38f * Sky + 0.9f * Lit * ToSun;
        }
        if (bInside)
            for (int k = 0; k < 3; k++)
                C[k].Light = 1.0f;
        GTriNow = bLeaf ? -1 : t / 3;
        if (bIn)
            DrawTriangle(Img, C[0], C[1], C[2], bInside ? GHole : Tex);
    }
}

static bool WriteBmp(const char* Path, const FImage& Img, int Down)
{
    const int W = Img.Width / Down, H = Img.Height / Down;
    const int Row = (W * 3 + 3) & ~3;
    FILE* F = nullptr;
    if (fopen_s(&F, Path, "wb") || !F)
        return false;
    BITMAPFILEHEADER Fh = {};
    BITMAPINFOHEADER Ih = {};
    Fh.bfType    = 0x4D42;
    Fh.bfOffBits = sizeof(Fh) + sizeof(Ih);
    Fh.bfSize    = Fh.bfOffBits + (DWORD)(Row * H);
    Ih.biSize    = sizeof(Ih);
    Ih.biWidth   = W;
    Ih.biHeight  = H;
    Ih.biPlanes  = 1;
    Ih.biBitCount = 24;
    fwrite(&Fh, sizeof(Fh), 1, F);
    fwrite(&Ih, sizeof(Ih), 1, F);
    unsigned char* Line = (unsigned char*)calloc((size_t)Row, 1);
    for (int y = H - 1; y >= 0; y--)
    {
        for (int x = 0; x < W; x++)
        {
            float Sum[3] = { 0.0f, 0.0f, 0.0f };
            for (int j = 0; j < Down; j++)
                for (int i = 0; i < Down; i++)
                {
                    const float* const C = Img.Colour + 3 * ((y * Down + j) * Img.Width + x * Down + i);
                    for (int c = 0; c < 3; c++)
                        Sum[c] += C[c];
                }
            for (int c = 0; c < 3; c++)
            {
                // To the screen's curve, as the game's linear pipeline's output would be.
                float V = Sum[c] / (float)(Down * Down);
                V       = (V <= 0.0031308f) ? 12.92f * V : 1.055f * powf(V, 1.0f / 2.4f) - 0.055f;
                Line[3 * x + (2 - c)] = Byte(V);
            }
        }
        fwrite(Line, 1, (size_t)Row, F);
    }
    free(Line);
    fclose(F);
    return true;
}

struct FKindSize
{
    EKind       Kind;
    const char* Name;
    float       Height, Radius;     // the class's own size: twice the collision height, the crown's half-width
    float       Foot;               // the game's own trunk's radius at its foot, to stand round (0: none)
};

static const FKindSize GKinds[] = {
    { Kind_Tree1, "Tree1", 250.0f, 100.0f, 12.5f },
    { Kind_Tree2, "Tree2", 365.0f, 120.0f, 30.0f },
    { Kind_Tree3, "Tree3", 249.0f, 125.0f, 19.0f },
    { Kind_Tree4, "Tree4", 377.0f, 170.0f, 39.0f },
    { Kind_Bush1, "Bushes1", 88.0f, 53.0f, 0.0f },
    { Kind_Bush2, "Bushes2", 70.0f, 32.0f, 0.0f },
    { Kind_Bush3, "Bushes3", 60.0f, 45.0f, 0.0f },
};

int main(int argc, char** argv)
{
    const char* Out   = ".";
    int         Seeds = 2;
    bool        bOwn  = false;
    const char* Level = nullptr;
    bool        bWhy  = false;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-own"))
            bOwn = true;
        else if (!strcmp(argv[i], "-level") && i + 1 < argc)
            Level = argv[++i];
        else if (!strcmp(argv[i], "-why"))
            bWhy = true;
        if (!strcmp(argv[i], "-out") && i + 1 < argc)
            Out = argv[++i];
        else if (!strcmp(argv[i], "-seeds") && i + 1 < argc)
            Seeds = atoi(argv[++i]);
    }
    CreateDirectoryA(Out, nullptr);

    // The two pictures the renderer draws for the plants, from the same code.
    static unsigned char BarkPixels[256 * 512 * 4];
    static unsigned char LeafPixels[2048 * 1024 * 4];
    DrawBark(BarkPixels, 256, 512);
    DrawLeaves(LeafPixels, 2048, 1024);
    const FTexture Bark  = { 256, 512, BarkPixels, true };
    const FTexture Leafs = { 2048, 1024, LeafPixels, false };
    // EZ-Tree's leaf cards (scripts/make_leaf_cards.py): the sheet beside the renderer, read from the project's assets.
    static unsigned char CardPixels[4096 * 1024 * 4];
    bool bCards = false;
    {
        char Exe[MAX_PATH], Path[MAX_PATH];
        GetModuleFileNameA(nullptr, Exe, MAX_PATH);
        char* const Cut = strrchr(Exe, '\\');
        if (Cut)
            *Cut = 0;
        sprintf_s(Path, "%s\\..\\..\\assets\\SecondRevision-leafcards.bin", Exe);
        FILE* F = nullptr;
        if (fopen_s(&F, Path, "rb") == 0 && F)
        {
            char Magic[8];
            int  Head[4];
            if (fread(Magic, 1, 8, F) == 8 && !memcmp(Magic, "RDXLEAF1", 8) && fread(Head, 4, 4, F) == 4 &&
                Head[0] == 4096 && Head[1] == 1024 && Head[2] == 4096 * 1024 * 4)
            {
                unsigned char* const Packed = (unsigned char*)malloc((size_t)Head[3]);
                bCards = Packed && fread(Packed, 1, (size_t)Head[3], F) == (size_t)Head[3] &&
                         RdxArchive::Inflate(Packed, Head[3], CardPixels, Head[2]);
                free(Packed);
            }
            fclose(F);
        }
        printf("leaf cards: %s\n", bCards ? "read" : "NOT read - the EZ-Tree kinds go the own way");
        if (bOwn)
        {
            bCards = false;
            printf("-own: every kind the own way\n");
        }
    }
    const FTexture Cards = { 4096, 1024, CardPixels, false };
    {
        // The pictures themselves: the bark, and the leaves' sheet at half its size.
        FImage Sheet;
        Sheet.Make(1280, 512);
        for (int y = 0; y < 512; y++)
            for (int x = 0; x < 1280; x++)
            {
                float* const C = Sheet.Colour + 3 * (y * 1280 + x);
                const unsigned char* P = (x < 256) ? BarkPixels + 4 * (y * 256 + x)
                                                   : LeafPixels + 4 * ((2 * y) * 2048 + 2 * (x - 256));
                const float A = (x < 256) ? 1.0f : (float)P[3] / 255.0f;
                for (int c = 0; c < 3; c++)
                {
                    // Into the linear values WriteBmp turns back; the leaves over a grey where they are not.
                    const float S = (float)P[c] / 255.0f;
                    const float L = (S <= 0.04045f) ? S / 12.92f : powf((S + 0.055f) / 1.055f, 2.4f);
                    C[c] = L * A + 0.2f * (1.0f - A);
                }
            }
        char Path[MAX_PATH];
        sprintf_s(Path, "%s\\pictures.bmp", Out);
        WriteBmp(Path, Sheet, 1);
    }

    static FGrid Grid;
    LARGE_INTEGER Freq;
    QueryPerformanceFrequency(&Freq);
    double TotalMs = 0.0;
    for (const FKindSize& K : GKinds)
        for (int s = 0; s < Seeds; s++)
        {
            unsigned int Seed  = 1000u + (unsigned int)s * 7919u + (unsigned int)K.Kind * 104729u;
            FShape       Shape = ShapeOf(K.Kind, K.Height, K.Radius);
            if (Level)
            {
                unsigned int Hash = 2166136261u;
                for (const char* C = Level; *C; C++)
                    Hash = (Hash ^ (unsigned int)((*C >= 'A' && *C <= 'Z') ? *C + ('a' - 'A') : *C)) * 16777619u;
                Seed = Hash ^ (unsigned int)((int)K.Kind * 7919 + s * 104729);
                if (K.Kind == Kind_Tree1 || K.Kind == Kind_Tree3)
                {
                    // Hell's Kitchen's: kind 0 from Tree4, 250 tall and 219 across; kind 2 from Tree1, 289 and 210.
                    const float Tall = (K.Kind == Kind_Tree1) ? 250.0f : 289.0f;
                    Shape           = ShapeOf(K.Kind, Tall, ((K.Kind == Kind_Tree1) ? 109.5f : 105.0f) * 1.1f);
                    Shape.CrownBase = 0.15f;
                    Shape.Widest    = 0.75f;
                    for (int b = 0; b < 10; b++)
                        Shape.Bark[b] = TrunkFoot(Tall) * (1.0f - 0.05f * (float)b);
                }
            }
            // A park tree's trunk for its height (TrunkFoot), as the renderer gives it; a bush has stems of its own.
            for (int b = 0; b < 10 && !(Level && (K.Kind == Kind_Tree1 || K.Kind == Kind_Tree3)); b++)
                Shape.Bark[b] = (K.Foot > 0.0f) ? TrunkFoot(K.Height) * (1.0f - 0.05f * (float)b) : 0.0f;
            FPlant             Plant;
            FMesh              Mesh;
            LARGE_INTEGER      T0, T1, T2;
            QueryPerformanceCounter(&T0);
            const bool bEz    = bCards && EzGrows(K.Kind);
            Shape.Cards       = bCards && Shape.Box > 0.0f;
            const bool bGrown = bEz ? GrowEz(Shape, Seed, Plant, Mesh, 48.0f) : Grow(Shape, Seed, Plant, Grid);
            QueryPerformanceCounter(&T1);
            const bool bBuilt = bGrown && (bEz || Build(Plant, Mesh, 48.0f));
            QueryPerformanceCounter(&T2);
            const double GrowMs  = 1000.0 * (double)(T1.QuadPart - T0.QuadPart) / (double)Freq.QuadPart;
            const double BuildMs = 1000.0 * (double)(T2.QuadPart - T1.QuadPart) / (double)Freq.QuadPart;
            TotalMs += GrowMs + BuildMs;
            printf("%-8s seed %u: %s, %d nodes, %d leaves; bark %d triangles, leaves %d; grown in %.1f ms, "
                   "built in %.1f ms\n",
                   K.Name, s, (bGrown && bBuilt) ? "ok" : "FAILED", Plant.Nodes.Num, Plant.Leaves.Num,
                   Mesh.BarkIndex.Num / 3, Mesh.Leaf.Num * (LeafFrameIndices / 3), GrowMs, BuildMs);
            if (!bBuilt)
                continue;
            // How open the crown is round its leaves (FOpenness): the tenth, the half and the ninth tenth of them, all
            // round and towards the sun - what decides how deep its shade reads.
            {
                int Round[256] = {}, ToSun[256] = {};
                const FVec Sun = Unit(GSun, Vec(0.0f, 0.0f, 1.0f));
                for (int l = 0; l < Mesh.Leaf.Num; l++)
                {
                    Round[Mesh.Leaf[l].Open[0]]++;
                    ToSun[(int)(OpenTowards(Mesh.Leaf[l].Open, Sun) * 255.0f + 0.5f)]++;
                }
                float Marks[2][3];
                for (int h = 0; h < 2; h++)
                {
                    const int* const Hist = h ? ToSun : Round;
                    const float      Want[3] = { 0.1f, 0.5f, 0.9f };
                    for (int w = 0; w < 3; w++)
                    {
                        int Sum = 0, b = 0;
                        while (b < 255 && Sum + Hist[b] < (int)(Want[w] * (float)Mesh.Leaf.Num))
                            Sum += Hist[b++];
                        Marks[h][w] = (float)b / 255.0f;
                    }
                }
                printf("         open round its leaves %.2f / %.2f / %.2f, towards the sun %.2f / %.2f / %.2f\n",
                       Marks[0][0], Marks[0][1], Marks[0][2], Marks[1][0], Marks[1][1], Marks[1][2]);
            }
            // The leaves' blades, as the vertex shader lays them (LeafCorner), into triangles of their own.
            TList<FVertex>      LeafVerts;
            TList<unsigned int> LeafIndex;
            for (int l = 0; l < Mesh.Leaf.Num; l++)
            {
                const unsigned int First = (unsigned int)LeafVerts.Num;
                for (int c = 0; c < LeafCorners; c++)
                {
                    FVec    Pos, Nrm;
                    FVertex V = {};
                    LeafCorner(Mesh.Leaf[l], c, Pos, Nrm, V.U, V.V, (bEz || Shape.Cards) ? 1.0f : 0.5f);
                    V.X = Pos.X; V.Y = Pos.Y; V.Z = Pos.Z;
                    V.NX = Nrm.X; V.NY = Nrm.Y; V.NZ = Nrm.Z;
                    for (int o = 0; o < 4; o++)
                        V.Open[o] = Mesh.Leaf[l].Open[o];
                    LeafVerts.Add(V);
                }
                for (int k = 0; k < LeafFrameIndices; k++)
                    LeafIndex.Add(First + LeafFrame[k]);
            }
            // Two sides, the trunk from near, the crown's edge from near and the wood where the trunk forks from near,
            // leafless; each twice the picture's size each way, made smaller when written.
            const int Side = 520, Down = 2;
            int       Holes[5] = {};
            FImage    Img;
            Img.Make(Side * 5 * Down, (int)(Side * 1.25f) * Down);
            for (int v = 0; v < 5; v++)
            {
                FImage Part;
                Part.Make(Side * Down, (int)(Side * 1.25f) * Down);
                const bool  bNear  = (v == 2);
                const bool  bLeafy = (v == 3);
                const bool  bFork  = (v == 4);
                const float Turn   = 0.6f + 1.5708f * (float)((bNear || bLeafy || bFork) ? 0 : v);
                FCamera     Cam;
                FVec        Look;
                if (bFork)
                {
                    // From a little below the crown's foot, looking up at it - and above the ground, as a player is: a
                    // bush's crown starts at the ground, and from under it the stems' open feet would be in view.
                    const float Up = fmaxf(Shape.CrownBase, 0.1f) * Shape.Height;
                    Look    = Vec(0.0f, 0.0f, Up);
                    Cam.Eye = Vec(cosf(Turn), sinf(Turn), -0.35f) * (0.3f * Shape.Height) + Look;
                    Cam.Eye.Z = fmaxf(Cam.Eye.Z, 2.0f);
                }
                else if (bLeafy)
                {
                    // A spot on the crown's side, from a little way out.
                    const float Up = Shape.CrownBase * Shape.Height + 0.45f * (1.0f - Shape.CrownBase) * Shape.Height;
                    Look    = Vec(cosf(Turn) * Shape.Radius * 0.7f, sinf(Turn) * Shape.Radius * 0.7f, Up);
                    Cam.Eye = Look + Vec(cosf(Turn), sinf(Turn), 0.15f) * (Shape.LeafSize * 9.0f);
                }
                else
                {
                    const float Far = bNear ? 0.36f * Shape.Height : 1.45f * fmaxf(Shape.Height, 2.0f * Shape.Radius) + 40.0f;
                    Look    = Vec(0.0f, 0.0f, (bNear ? 0.16f : 0.48f) * Shape.Height);
                    Cam.Eye = Vec(cosf(Turn) * Far, sinf(Turn) * Far, (bNear ? 0.12f : 0.32f) * Shape.Height + 20.0f);
                }
                Cam.Ahead = Unit(Look - Cam.Eye, Vec(1.0f, 0.0f, 0.0f));
                Cam.Right = Unit(Cross(Cam.Ahead, Vec(0.0f, 0.0f, 1.0f)), Vec(1.0f, 0.0f, 0.0f));
                Cam.Up    = Cross(Cam.Right, Cam.Ahead);
                Cam.Focal = (float)Part.Height * 0.95f;
                Cam.Cx    = (float)Part.Width * 0.5f;
                Cam.Cy    = (float)Part.Height * 0.5f;
                Background(Part, Cam);
                if (bWhy)
                {
                    GTriAt = (int*)malloc((size_t)Part.Width * Part.Height * sizeof(int));
                    for (int q = 0; q < Part.Width * Part.Height; q++)
                        GTriAt[q] = -1;
                }
                DrawMesh(Part, Cam, Mesh.Bark, Mesh.BarkIndex, Bark, false);
                if (!bFork)
                    DrawMesh(Part, Cam, LeafVerts, LeafIndex, (bEz || Shape.Cards) ? Cards : Leafs, true);
                Holes[v] = HolePixels(Part);
                if (GTriAt)
                {
                    // The red pixels' triangles, counted; the five most seen told by the node whose tube is nearest.
                    TList<int> Seen;
                    for (int q = 0; q < Part.Width * Part.Height; q++)
                        if (GTriAt[q] >= 0 && Part.Colour[3 * q] > 0.99f && Part.Colour[3 * q + 1] < 0.001f &&
                            Part.Depth[q] > HoleNearest)
                            Seen.Add(GTriAt[q]);
                    for (int Told = 0; Told < 5 && Seen.Num > 0; Told++)
                    {
                        int Best = -1, BestN = 0;
                        for (int a = 0; a < Seen.Num; a++)
                        {
                            if (Seen[a] < 0)
                                continue;
                            int n = 0;
                            for (int b = 0; b < Seen.Num; b++)
                                n += (Seen[b] == Seen[a]) ? 1 : 0;
                            if (n > BestN)
                            {
                                BestN = n;
                                Best  = Seen[a];
                            }
                        }
                        if (Best < 0)
                            break;
                        for (int a = 0; a < Seen.Num; a++)
                            if (Seen[a] == Best)
                                Seen[a] = -1;
                        const FVertex& A0 = Mesh.Bark[Mesh.BarkIndex[3 * Best]];
                        const FVec     Mid = Vec(A0.X, A0.Y, A0.Z);
                        int   Node = -1;
                        float Near = 1e30f;
                        for (int n = 0; n < Plant.Nodes.Num; n++)
                        {
                            const int Pn = Plant.Nodes[n].Parent;
                            if (Pn < 0)
                                continue;
                            const FVec  Ab = Plant.Nodes[n].At - Plant.Nodes[Pn].At;
                            const float L2 = Dot(Ab, Ab);
                            float       T  = (L2 > 1e-6f) ? Dot(Mid - Plant.Nodes[Pn].At, Ab) / L2 : 0.0f;
                            T              = (T < 0.0f) ? 0.0f : ((T > 1.0f) ? 1.0f : T);
                            const float D  = Length(Mid - (Plant.Nodes[Pn].At + Ab * T)) -
                                            (Plant.Nodes[Pn].Radius + (Plant.Nodes[n].Radius - Plant.Nodes[Pn].Radius) * T);
                            if (fabsf(D) < Near)
                            {
                                Near = fabsf(D);
                                Node = n;
                            }
                        }
                        const FNode& Nd  = Plant.Nodes[Node];
                        const FNode& Pd  = Plant.Nodes[Nd.Parent];
                        const FVec   Ab  = Nd.At - Pd.At;
                        const float  T   = Dot(Mid - Pd.At, Ab) / fmaxf(Dot(Ab, Ab), 1e-6f);
                        const FVec   Rad = Mid - (Pd.At + Ab * T);
                        int Kids = 0, Thick = -1;
                        for (int n = 0; n < Plant.Nodes.Num; n++)
                            if (Plant.Nodes[n].Parent == Nd.Parent && (Thick < 0 || Plant.Nodes[n].Radius > Plant.Nodes[Thick].Radius))
                                Thick = n;
                        for (int n = 0; n < Plant.Nodes.Num; n++)
                            Kids += (Plant.Nodes[n].Parent == Node) ? 1 : 0;
                        printf("           view %d: %d red px of triangle %d - node %d (radius %.2f, its parent %d %.2f, "
                               "%s child, %d children of its own), %.2f along, %.2f off the wood's girth; normal %s, "
                               "%.0f units up\n",
                               v, BestN, Best, Node, Nd.Radius, Nd.Parent, Pd.Radius,
                               (Thick == Node) ? "the thickest" : "another", Kids, T, Near,
                               (Dot(Vec(A0.NX, A0.NY, A0.NZ), Rad) > 0.0f) ? "outward" : "INWARD", Mid.Z);
                    }
                    free(GTriAt);
                    GTriAt = nullptr;
                }
                for (int y = 0; y < Part.Height; y++)
                    memcpy(Img.Colour + 3 * (y * Img.Width + v * Part.Width), Part.Colour + 3 * (y * Part.Width),
                           sizeof(float) * 3 * (size_t)Part.Width);
            }
            // The bark's inside in view, in the picture's pixels (before it is made smaller): the two sides, the trunk
            // from near, the crown's edge, the fork from below.
            printf("         holes (the bark's inside in view, pixels): sides %d / %d, trunk %d, crown %d, fork %d\n",
                   Holes[0], Holes[1], Holes[2], Holes[3], Holes[4]);
            char Path[MAX_PATH];
            sprintf_s(Path, "%s\\%s-%d.bmp", Out, K.Name, s);
            if (!WriteBmp(Path, Img, Down))
                printf("  could not write %s\n", Path);
        }
    printf("all plants: %.0f ms\n", TotalMs);
    return 0;
}

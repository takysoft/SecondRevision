// SecondRevision - the arithmetic and classification the render device depends on.
//
// This header deliberately depends on NOTHING: no engine headers, no Windows, no
// Direct3D - its one include is the compiler's own SSE2 header, for the dilation. Every
// rule the game imposes on us that can be expressed as "inputs in, answer out" lives
// here, so it can be tested in milliseconds without a window, a GPU, or the game itself.
//
// That matters because this is where the real bugs were. Every visual defect found so
// far traced back to one of these expressions being subtly wrong - the half-texel
// light map shift, the blend classification, the palette's transparent entry - and
// none of them needed a renderer to reproduce.
//
// Every value here is either read from the Ion Storm / Epic SDK headers or measured
// with our own instrument; the docs/ reference is given at each one.
#pragma once

// The numbers the shader files share with this side (RDX_...), as macros.
#include "RenderLimits.h"

// SSE2: every processor that runs this game has it. Elsewhere the dilation reads texel by
// texel, with the same result.
#ifndef RDX_DILATE_SSE2
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE2__)
#define RDX_DILATE_SSE2 1
#else
#define RDX_DILATE_SSE2 0
#endif
#endif
#if RDX_DILATE_SSE2
#include <emmintrin.h>
#endif

namespace RdxMath
{

// ---------------------------------------------------------------------------
// Small plain types. Not the engine's - that is the point.
// ---------------------------------------------------------------------------

struct FVec3
{
    float X, Y, Z;
};

// A square root WITHOUT a library. This header includes none on purpose (see the top), and most of the arithmetic here
// avoids the root altogether - SphereTouchesView compares squares for exactly that reason. Where a root is unavoidable
// (ShadowCanReachView needs a unit direction), the SSE2 header already taken at the top carries the instruction itself,
// so this costs one machine instruction and no dependency. The fallback is Newton's method, which converges to float
// precision from any positive start; it is there for completeness, not for this project's x86 build.
inline float Sqrt(float V)
{
    if (!(V > 0.0f))
        return 0.0f;
#if RDX_DILATE_SSE2
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(V)));
#else
    float X = (V < 1.0f) ? 1.0f : V;
    for (int i = 0; i < 24; i++)
        X = 0.5f * (X + V / X);
    return X;
#endif
}

struct FRgba
{
    unsigned char R, G, B, A;
};

inline float Dot(const FVec3& A, const FVec3& B)
{
    return A.X * B.X + A.Y * B.Y + A.Z * B.Z;
}

// ---------------------------------------------------------------------------
// Polygon flags
// ---------------------------------------------------------------------------
//
// Mirrors of the engine's EPolyFlags values (docs/spec-02 section 1). They are
// repeated here so this header stays dependency-free; RenderDevice.cpp holds
// static assertions that they still equal the SDK's, so the two cannot drift.

enum EPolyFlagBits
{
    RDX_PF_Masked      = 0x00000002,
    RDX_PF_Translucent = 0x00000004,
    RDX_PF_Modulated   = 0x00000040,
    RDX_PF_Unlit       = 0x00400000,
    RDX_PF_FakeBackdrop= 0x00000080,
};

// PF_Occlude is the top bit; kept out of the enum so it stays unsigned.
const unsigned int RDX_PF_Occlude = 0x80000000u;

enum EBlend
{
    Blend_Opaque = 0,   // masked pixels discarded, depth written
    Blend_Translucent,  // src + dst*(1-src), measured (docs/meres-07 13): black adds nothing, so fades work
    Blend_Modulated     // multiplies what is behind it
};

// Translucent wins over modulated when both are set: a surface that asks to be seen
// through should not instead darken what is behind it.
inline EBlend BlendFor(unsigned int PolyFlags)
{
    if (PolyFlags & RDX_PF_Translucent)
        return Blend_Translucent;
    if (PolyFlags & RDX_PF_Modulated)
        return Blend_Modulated;
    return Blend_Opaque;
}

// Does the vertex colour take part in this draw?
//
// Not for a modulated one. Measured: the sniper scope's overlay is handed to us with
// colour (0,0,0) - multiplying by that turned the entire scope black, which is exactly
// what it looked like. A modulated draw multiplies what is behind it by the texture, and
// by nothing else.
inline bool UsesVertexColour(EBlend Mode)
{
    return Mode != Blend_Modulated;
}

// Is this texture a colour, or a number?
//
// A modulated texture is a MULTIPLIER applied to what is already on screen: 128 means
// "leave it alone". Linearising it as if it were colour turns that 0.5 into 0.216, and
// every modulated draw then darkens the picture instead of leaving it be. Measured
// consequence: a grey box around the mouse cursor, square dark patches where blood
// decals should be, and a sniper scope that came out at exactly (0,0,0).
inline bool SamplesAsColour(EBlend Mode)
{
    return Mode != Blend_Modulated;
}

// Surfaces that carry their own brightness must not be darkened by a light map: an unlit surface should not be lit,
// and the sky (FakeBackdrop) carries its own.
//
// Worth knowing how little this is worth in practice: switching it on moves the difference from the reference picture
// by 0.01. It is right, not load-bearing - so a visual fault is unlikely to be waiting here.
inline bool WantsLightMap(unsigned int PolyFlags)
{
    return (PolyFlags & (RDX_PF_Unlit | RDX_PF_FakeBackdrop)) == 0;
}

// Does this draw belong in the depth buffer?
//
// Ordinary opaque world geometry does. A two-dimensional tile never does: it is an
// overlay painted in the engine's own order, and the engine clears depth before the 2D
// pass anyway (measured: ClearZ once per frame, after the world, before the HUD).
//
// A tile that writes depth becomes an occluder for everything drawn after it. Measured
// consequence: with the sniper scope raised, the zoomed world was submitted normally -
// 4154 triangles in 327 draws - and 99.6% of the view came out at exactly (0,0,0),
// because a full-screen backdrop tile had already claimed the depth in front of it.
inline bool WritesDepth(EBlend Mode, bool bTile)
{
    return !bTile && Mode == Blend_Opaque;
}

// A BSP surface the level marks PF_Occlude ("occludes even if PF_NoOcclude", SDK) is an
// occluder even when it is translucent. Measured: the reflective UNATCO floor arrives as
// 0x88800004 - Occlude + Mirrored + HighShadowDetail + Translucent - and a ceiling fan
// hanging in the room UNDER it, sent in the main view (not in the mirror's node), showed
// through exactly where the floor is reflective, because the translucent floor wrote no
// depth. The stock renderer hides it. Only the translucent case is handled: no modulated
// occluder has been seen.
inline bool WritesDepth(EBlend Mode, bool bTile, unsigned int PolyFlags)
{
    if (bTile)
        return false;
    if (Mode == Blend_Opaque)
        return true;
    return Mode == Blend_Translucent && (PolyFlags & RDX_PF_Occlude) != 0;
}

// Which tiles are the HUD (docs/terv-latvany-roadmap.md 1.3). Measured over four scenes - the fan room with
// its mirror, Liberty with the drone's camera open, the ESC menu, the inventory: every tile of the HUD, the
// menus and the inventory arrived at Z <= 1 in a main scene node (153, 251, 443 and 223 a frame), every sprite
// placed in the world deeper than 10 (36-76 a frame in Liberty), and no tile at all at Z <= 0 or between 1
// and 10. A tile in a child node (a mirror) belongs to that node's picture, whatever its depth.
inline bool IsHudTile(bool bInChildNode, float Z)
{
    return !bInChildNode && Z <= 1.0f;
}

// Light inventory (docs/terv-latvany-roadmap.md 3.1): does a light's sphere of influence touch the view? The centre is
// in camera space (UE1: X right, Y down, Z forward), the frustum symmetric, TanX and TanY the tangents of its half
// angles. A side plane passes through the eye, and the signed distance of the centre from the right one is
// (X - Z * TanX) / sqrt(1 + TanX^2): the sphere reaches inside when that is at most R. Squared, so no square root is
// needed. No far plane - the engine draws to the horizon. Behind the eye only a sphere around the camera counts.
inline bool SphereTouchesView(float X, float Y, float Z, float R, float TanX, float TanY)
{
    if (Z < -R)
        return false;
    const float AX = X < 0.0f ? -X : X;
    const float AY = Y < 0.0f ? -Y : Y;
    const float DX = AX - Z * TanX;
    const float DY = AY - Z * TanY;
    const bool bInX = DX <= 0.0f || DX * DX <= R * R * (1.0f + TanX * TanX);
    const bool bInY = DY <= 0.0f || DY * DY <= R * R * (1.0f + TanY * TanY);
    return bInX && bInY;
}

// The same question for ONE TILE of the screen instead of the whole view - the measurement that says whether a tiled
// light list would be worth building (docs/arnyek-atadas.md 8.). SphereTouchesView cannot be reused: it takes the
// centre's ABSOLUTE value, so it only knows symmetric frusta, while a tile is an asymmetric slice of the screen and
// each of its four side planes is its own. Same principle otherwise - a side plane through the eye, the centre's
// signed distance from it, compared squared so no square root is needed.
// TanL/TanR are the tangents of the tile's left and right edges (TanL < TanR, signed, at Z = 1), TanT/TanB the top and
// bottom ones. Camera space is UE1's: X right, Y DOWN, Z forward, so the top edge's tangent is the smaller one.
// Conservative in the same direction as the rest: it never says no to a light that does reach the tile.
inline bool SphereTouchesTile(float X, float Y, float Z, float R,
                              float TanL, float TanR, float TanT, float TanB)
{
    if (Z < -R)
        return false;
    const float RR = R * R;
    const float DR = X - Z * TanR;                       // past the right edge
    if (DR > 0.0f && DR * DR > RR * (1.0f + TanR * TanR))
        return false;
    const float DL = Z * TanL - X;                       // past the left edge
    if (DL > 0.0f && DL * DL > RR * (1.0f + TanL * TanL))
        return false;
    const float DB = Y - Z * TanB;                       // past the bottom edge
    if (DB > 0.0f && DB * DB > RR * (1.0f + TanB * TanB))
        return false;
    const float DT = Z * TanT - Y;                       // past the top edge
    if (DT > 0.0f && DT * DT > RR * (1.0f + TanT * TanT))
        return false;
    return true;
}

// Screen-space shadows' light pick (RenderDevice.cpp, RankShadowLights): how much of a light's share a caster standing at
// Dist from it could take away - the same estimate the factor shader makes for a surface: Bright01 * (1 - Dist/Radius)^2,
// and for a spot the fade from its cone's edge (CosHalfAngle) to its axis. 0 outside the light's reach or its cone.
// Measured reason: picked by brightness and size alone, the four lights were far off
// or behind the camera, none near the soldiers - and the factor view was white.
inline float ShadowPotential(float Bright01, float Dist, float Radius, bool bSpot, float CosToCaster, float CosHalfAngle)
{
    if (Radius <= 0.0f || Dist >= Radius)
        return 0.0f;
    const float Att = 1.0f - Dist / Radius;
    float P = Bright01 * Att * Att;
    if (bSpot)
    {
        const float Den = (1.0f - CosHalfAngle > 0.001f) ? (1.0f - CosHalfAngle) : 0.001f;
        float F = (CosToCaster - CosHalfAngle) / Den;
        F = (F < 0.0f) ? 0.0f : ((F > 1.0f) ? 1.0f : F);
        P *= F;
    }
    return P;
}

// The shadows' share of a pixel's light (the factor shader, relative mode): a light's estimated contribution - the same
// estimate, times Calib to bring it to the light target's units - against all the light the pixel receives (the geometry
// pass's light target), capped at MaxShare. Measured reason: the absolute share of a dim
// light (brightness 16, radius 3225) came to ~3%, although that light is nearly all the dark street has.
inline float RelativeShadowShare(float Contribution, float Total, float Calib, float MaxShare)
{
    const float T = (Total > 0.02f) ? Total : 0.02f;
    float S = Calib * Contribution / T;
    S = (S < 0.0f) ? 0.0f : ((S > 1.0f) ? 1.0f : S);
    return S * MaxShare;
}

// ---------------------------------------------------------------------------
// Depth
// ---------------------------------------------------------------------------

// Reversed-Z: 1 at the near plane, approaching 0 with distance. Paired with a float
// depth buffer this puts the precision where the 115..3000 unit view depths the
// engine actually uses need it (docs/spec-03 section 2).
inline float DepthFromViewZ(float ViewZ)
{
    return (ViewZ > 1.0f) ? (1.0f / ViewZ) : 1.0f;
}

// ---------------------------------------------------------------------------
// Perspective
// ---------------------------------------------------------------------------

// The engine hands the device vertices that are ALREADY projected to screen pixels, so
// there is no matrix to apply - but the perspective divide still has to be undone and
// redone. If the clip-space W is left at 1, the rasteriser interpolates every attribute
// linearly in SCREEN space. That is affine mapping, and its measured consequence is that
// textures warp on oblique surfaces and swim as the camera moves (docs/spec-03 section 4,
// point 2, which predicted this before we built it).
//
// W must be the view-space depth. Our vertices carry its reciprocal in Z.
inline float PerspectiveW(float Depth)
{
    return (Depth > 1e-6f) ? (1.0f / Depth) : 1.0f;
}

// What the hardware computes for an attribute at screen parameter T along an edge once W
// is correct: attributes are interpolated as A/W and divided by 1/W per pixel.
inline float PerspectiveLerp(float A0, float W0, float A1, float W1, float T)
{
    const float InvW = (1.0f - T) / W0 + T / W1;
    const float Over = (1.0f - T) * (A0 / W0) + T * (A1 / W1);
    return (InvW != 0.0f) ? (Over / InvW) : A0;
}

// Every two-dimensional tile - HUD, menu, text, cursor - is handed to us at the same
// depth (measured: Z is a constant 1.0, docs/meres-01 section 3b). The engine draws them
// in painter's order and expects later ones to cover earlier ones. A strict comparison
// rejects the second tile at a given pixel, because its depth is not GREATER than the
// one already stored, only equal. Measured consequence: menu labels disappeared the
// moment they sat on a highlighted button, and the mouse cursor drew a box around itself.
inline bool DepthPasses(float Incoming, float Stored)
{
    return Incoming >= Stored;
}

// ---------------------------------------------------------------------------
// Texture coordinates for BSP surfaces (docs/spec-04)
// ---------------------------------------------------------------------------
//
// The engine gives no UVs on this path - only a mapping frame and world points.
// The mapping axes are NOT unit vectors: their length carries the texture scale, so
// projecting a point onto them already yields texels.

inline float SurfaceCoord(const FVec3& Axis, const FVec3& Origin, const FVec3& Point, float Pan)
{
    return Dot(Axis, Point) - Dot(Axis, Origin) - Pan;
}

// A light map does not tile: exactly one covers the surface, and its own scale is
// part of the divisor.
inline float LightDivisor(float Scale, int Size)
{
    const float D = Scale * (float)Size;
    return (D != 0.0f) ? D : 1.0f;
}

// Half a light map texel, subtracted from the pan.
//
// The raw projection addresses texel CORNERS, not centres: measured, the computed
// range stops at exactly (Size-1)/Size of the map. Shifting by half a texel moves
// every sample to a texel centre. Without it the lighting slides across the geometry.
inline float LightPan(float Pan, float Scale)
{
    return Pan - 0.5f * Scale;
}

// ---------------------------------------------------------------------------
// Palette expansion
// ---------------------------------------------------------------------------

// Palette index 0 is the engine's transparent entry for masked surfaces. The entry
// still holds a real colour - it simply must not be drawn - so only the alpha changes.
inline FRgba PaletteTexel(unsigned char Index, const FRgba& Entry, bool bMasked)
{
    FRgba Out;
    Out.R = Entry.R;
    Out.G = Entry.G;
    Out.B = Entry.B;
    Out.A = (bMasked && Index == 0) ? 0 : 255;
    return Out;
}

// A texel as the 32-bit word the upload writes: R, G, B, A in that order in memory - the
// R8G8B8A8 texture's byte order, on a little-endian processor. One word per texel instead
// of four byte writes; see FTextureCache::Convert.
inline unsigned int PackRgba(const FRgba& T)
{
    return (unsigned int)T.R | ((unsigned int)T.G << 8) | ((unsigned int)T.B << 16) |
           ((unsigned int)T.A << 24);
}

// Seven bits per channel in a 32-bit word; doubling recovers the full range. The
// engine used the spare bit as headroom for lights that overbrighten.
inline unsigned char Rgba7Channel(unsigned char Value)
{
    const int V = (int)Value * 2;
    return (unsigned char)(V > 255 ? 255 : V);
}

// A light map's RGBA7 texels do NOT arrive in the same channel order as a palette entry.
//
// Measured, not assumed. Rendering the light alone gives our light; dividing the stock
// renderer's frame by our own diffuse-only frame gives the light IT uses. Correlating the
// two over 150000 pixels puts our red against its blue (0.40) and our blue against its red
// (0.32), while red against red is 0.04 - no relationship at all. The diffuse path reads
// palette entries through the same FColor and comes out correct, so this is the light map
// data's own byte order, not FColor's. The engine headers document neither.
inline FRgba LightMapTexel(const FRgba& Raw)
{
    FRgba Out;
    Out.R = Rgba7Channel(Raw.B);
    Out.G = Rgba7Channel(Raw.G);
    Out.B = Rgba7Channel(Raw.R);
    Out.A = 255;
    return Out;
}

// A row of RGBA7 texels - four bytes each, in memory order - expanded to R8G8B8A8 exactly as
// Rgba7Channel and LightMapTexel do it texel by texel: bytes 0..2 doubled, saturating at 255;
// byte 3 becomes 255 - or, with bKeepAlpha, is doubled as well: a fog map's alpha is how much of the
// surface its fog covers; and with bSwap02, bytes 0 and 2 change places (a light map's order - see
// LightMapTexel - given an FColor whose R, G, B are bytes 0, 1, 2; FTextureCache asserts that).
// Four texels at once where the processor has SSE2 (RDX_DILATE_SSE2, this header's switch): a
// saturating add of each byte to itself is exactly min(2v, 255). TestRgba7Rows compares the two
// for every byte value in every position. Measured before (Liberty, turning): the conversion
// was 0.030 of the 0.056 ms each new light map cost - texel by texel, through FRgba.
inline void ExpandRgba7Row(const unsigned char* Src, unsigned char* Dst, int Width, bool bSwap02,
                           bool bKeepAlpha = false)
{
    int x = 0;
#if RDX_DILATE_SSE2
    const __m128i Alpha = _mm_set1_epi32((int)0xFF000000u);
    const __m128i Rgb   = _mm_set1_epi32(0x00FFFFFF);
    const __m128i Byte0 = _mm_set1_epi32(0x000000FF);
    const __m128i Byte1 = _mm_set1_epi32(0x0000FF00);
    for (; x + 4 <= Width; x += 4)
    {
        __m128i V = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Src + x * 4));
        V = _mm_adds_epu8(V, V);
        const __m128i A = bKeepAlpha ? _mm_and_si128(V, Alpha) : Alpha;
        if (bSwap02)
            V = _mm_or_si128(_mm_and_si128(V, Byte1),
                             _mm_or_si128(_mm_and_si128(_mm_srli_epi32(V, 16), Byte0),
                                          _mm_slli_epi32(_mm_and_si128(V, Byte0), 16)));
        else
            V = _mm_and_si128(V, Rgb);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(Dst + x * 4), _mm_or_si128(V, A));
    }
#endif
    for (; x < Width; x++)
    {
        const unsigned char* S  = Src + x * 4;
        unsigned char*       D  = Dst + x * 4;
        const unsigned char  C0 = Rgba7Channel(S[0]);
        const unsigned char  C2 = Rgba7Channel(S[2]);
        D[0] = bSwap02 ? C2 : C0;
        D[1] = Rgba7Channel(S[1]);
        D[2] = bSwap02 ? C0 : C2;
        D[3] = bKeepAlpha ? Rgba7Channel(S[3]) : (unsigned char)255;
    }
}

// ---------------------------------------------------------------------------
// Light scale and its neutral
// ---------------------------------------------------------------------------

// How a light map value becomes a brightness multiplier: RDX_LIGHT_SCALE_VALUE, in RenderLimits.h - the shader files read
// it from there.

// Colour space of the whole pipeline. 0: textures are decoded from sRGB when sampled and
// the render target encodes on write, so every multiply and every blend happens in linear
// light. 1 ("classic"): nothing is converted and all of it happens on the encoded numbers
// - which is what the original engine did, and what its content was lit and tuned for.
// Both cannot match the stock renderer at once: a light map that darkens a texel to half
// halves the NUMBER in one and the LIGHT in the other.
//
// Measured, and classic won. Same saved games, nothing calibrated (light gain 1.0): the
// classic build lands within 0.84..1.01 of the stock renderer's pre-curve values on
// UNATCO's walls and at 1.00 on the HUD, where the linear one was 2.2..2.5 times too dark
// and off in hue. The linear path stays for later work (HDR output), not as the default.
// Zero in every normal build. Set to a frame number by a TEST build only, to take the device away on purpose at
// that frame - see the deliberate device loss in FBackend::EndFrame. It lives here beside the other build
// switches so that "what can this build do that the shipped one cannot" is answerable from one place.
#ifndef RDX_TEST_DEVICE_LOSS
#define RDX_TEST_DEVICE_LOSS 0
#endif

// Zero in every normal build. Set to a frame number by a TEST build only, to read that frame back through the same
// call the engine uses for screenshots and save snapshots, and write it out - see DumpReadPixels. It is a build
// switch and not a key because the path is otherwise only reachable by SAVING THE GAME, and the saves on the
// machine this runs on belong to the player.
#ifndef RDX_TEST_READPIXELS
#define RDX_TEST_READPIXELS 0
#endif

#ifndef RDX_CLASSIC
#define RDX_CLASSIC 1
#endif

// A single gain on everything that is LIGHT - baked light maps and per-vertex mesh
// lighting alike - and on nothing else. It must not touch the neutral map that geometry
// without lighting binds, or the HUD would darken with the world; that is why it lives in
// the vertex colour and not in the shader's light term.
//
// Calibrated against the stock renderer on the same saved game and the same spot: median
// ratio 0.495 / 0.484 / 0.400 across 265000 pixels (docs/meres-06).
#ifndef RDX_LIGHT_GAIN
#if RDX_CLASSIC
#define RDX_LIGHT_GAIN 1.0f     // classic: measured 0.84..1.01 on UNATCO walls - docs/meres-07
#else
#define RDX_LIGHT_GAIN 0.46f
#endif
#endif

inline float LightGain()
{
    return RDX_LIGHT_GAIN;
}

// Geometry that has no light map - every mesh, every 2D tile - binds a neutral one so the shader can multiply
// unconditionally with no branch. "Neutral" means the product with RDX_LIGHT_SCALE must come out at exactly one, so
// the texel is DERIVED from the scale here rather than written down beside it. Set by hand, the pair drifts apart the
// moment the scale is recalibrated, and every mesh in the game changes brightness while the lightmapped walls do not.
//
// A scale below one cannot be cancelled at all: the neutral would have to exceed 255. The function saturates there,
// and the tests record that as a limit rather than a fault.
inline unsigned char NeutralLightTexel(float LightScale)
{
    if (LightScale <= 0.0f)
        return 255;
    const float V = 255.0f / LightScale;
    return (unsigned char)(V >= 255.0f ? 255.0f : (V + 0.5f));
}

// Mesh vertices carry their own lighting instead of a light map, and it arrives as a
// float with no expansion step of its own. A light map's RGBA7 texels are stored in seven
// bits and doubled when unpacked - that doubling is where the engine's overbright factor
// lives for BSP surfaces. The gouraud path has to apply the same factor explicitly, or
// every mesh in the game renders at half the brightness of every wall.
//
// Measured before this existed: trees 2.76x, crates 1.76x and the player's weapon 2.84x
// darker than the stock renderer on the same saved game, while lightmapped walls were
// within 1.4x (docs/meres-04). Not clamped: an overbright light is allowed to exceed one,
// and the render target saturates it, which is what the engine intends.
#ifndef RDX_GOURAUD_SCALE
#if RDX_CLASSIC
#define RDX_GOURAUD_SCALE 1.0f
#else
#define RDX_GOURAUD_SCALE 2.0f
#endif
#endif

inline float GouraudLight(float Value)
{
    // NOT multiplied by the light gain. That gain calibrates the baked light maps, and
    // the whole-frame median it came from is dominated by walls and floors. Applying it
    // here as well made every mesh five times darker than the stock renderer - measured
    // on the player's weapon: reference (40, 27, 11) against our (8, 6, 2).
    //
    // The doubling belonged to the LINEAR pipeline. In the classic one it made the weapon
    // 2.2 times too bright against the stock renderer (ratio 0.46 / 0.44 in R / G on the
    // Liberty Island save); used as it comes, it lands at about 0.92.
    return Value * RDX_GOURAUD_SCALE;
}

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------

// The stock renderer's brightness: out = in ^ (1 / (2.5 * Brightness)) on the finished
// frame, HUD included. Measured, not looked up. Three captures of one frame at Brightness
// 0.5, 0.64 and 0.8 relate to each other exactly as 1/b predicts (0.782 and 0.625
// measured, 0.781 and 0.625 predicted), and the constant comes from 8077 flat, opaque HUD
// pixels of the classic build at 0.64: exponent 0.622, with no spread at all.
inline float OutputExponent(float Brightness)
{
    if (Brightness < 0.05f) Brightness = 0.05f;    // zero would divide by zero
    if (Brightness > 4.0f)  Brightness = 4.0f;
    return 1.0f / (2.5f * Brightness);
}

// ---------------------------------------------------------------------------
// Masked edges
// ---------------------------------------------------------------------------

// A masked texture's transparent texels still carry a real colour: in Deus Ex palette
// entry 0 is bright magenta, the traditional "do not draw this" key. Clearing alpha is
// not enough, because a bilinear sampler averages the RGB of neighbouring texels
// regardless of their alpha - so every silhouette picks up a magenta halo. Measured: the
// grass blades and the inventory icons were outlined in magenta, and thin icons were
// tinted purple outright.
//
// The cure is to hand each transparent texel the colour of its visible neighbours, so the
// filter has nothing wrong to pull in. Alpha is never touched, so the alpha test still
// discards exactly the same texels.
#ifndef RDX_DILATE_PASSES
#define RDX_DILATE_PASSES 4
#endif

// THE PASS COUNT IS A DISTANCE IN TEXELS. One pass fills only the texels immediately next to something visible, which
// is enough while a texture is magnified; minified, it is sampled across a wider footprint and reaches further out -
// the sniper icon in the tool bar keeps magenta along one edge after a single pass.
//
// For that to hold, a pass must read only texels that were visible BEFORE it began: opaque ones, or ones an earlier
// pass filled. Marking every fill with alpha 1 and reading those marks within the same pass looks equivalent and is
// not - the colour then runs on along the scan direction without limit and the count means nothing. So pass P marks
// with alpha P + 1 and skips its own marks. Every mark is cleared at the end; like zero, they all fail the alpha test.
//
// WRAPPING. The sampler wraps, because every UE1 texture tiles: the texel above the first row is the last row, and the
// one left of the first column is the last column, so a bilinear sample on an edge reaches across to the opposite
// edge. Stopping at the border leaves the seam magenta wherever a texture is visible along one edge and empty along
// the other - on the city sky line, a three-pixel dashed line across the bottom of the screen.
//
// Expects alpha to be either 0 (transparent) or above the pass count (visible), which the palette expansion
// guarantees: it only ever writes 0 and 255.
//
// SPEED. The plain form of this - scan the whole texture every pass and, for every transparent texel, read all eight
// neighbours, on every mip level - costs about 100 ms for one big masked texture, which is a whole frame, arriving
// mid-turn. This version gives the same result byte for byte for any input (TestDilateEquivalence compares the two):
//   - visibility is kept as one bit per texel, and each pass finds its candidates - the transparent texels with
//     something visible among their eight neighbours - 32 at a time;
//   - only the candidates read their neighbours, by columns shared with the candidate before, without a branch per
//     neighbour and without a division.
// The order in which a pass visits its candidates does not matter: a pass reads only texels that were visible before
// it began, and changes only texels that were transparent.

// The index of the lowest set bit of a non-zero word. The processor has an instruction for
// it, which the compiler exposes as a built-in - declared here rather than through a header,
// because this header includes no library (the declaration is the compiler's own, as is the
// SSE2 header at the top). Anywhere
// else: isolate the bit, then read its index one binary digit at a time; each mask holds
// the bit positions whose index has that digit set.
#if defined(_MSC_VER)
extern "C" unsigned char _BitScanForward(unsigned long* Index, unsigned long Mask);
#pragma intrinsic(_BitScanForward)
#endif
inline int DilateLowestBit(unsigned int V)
{
#if defined(_MSC_VER)
    unsigned long Index;
    _BitScanForward(&Index, V);
    return (int)Index;
#elif defined(__GNUC__)
    return __builtin_ctz(V);
#else
    const unsigned int L = V & (0u - V);
    return (int)((((L & 0xFFFF0000u) != 0) << 4) | (((L & 0xFF00FF00u) != 0) << 3) |
                 (((L & 0xF0F0F0F0u) != 0) << 2) | (((L & 0xCCCCCCCCu) != 0) << 1) |
                  ((L & 0xAAAAAAAAu) != 0));
#endif
}

// A texel's eight neighbours are three columns - left of it, its own, right of it - of
// three rows each, less the texel itself, which never counts anyway: it is transparent, or
// once filled carries this pass's mark. Neighbouring candidates in a row share two of their
// three columns, so a candidate next to the previous one needs one new column: three reads
// instead of eight. A column's sums cannot change while the pass runs, because the only
// texels a pass changes go from transparent to its own mark, and neither counts.
// On a texture one or two texels wide or high the wrap lands several of the nine on the
// same texel, which is then counted again - the same way the plain loop counts it.
//
// The sums of one column, over the texels visible before this pass began: alpha neither
// 0 (transparent) nor Mark (filled by this same pass). Red and blue share a word: a sum
// never exceeds 8 * 255, so the halves never meet.
struct FDilateColumn
{
    unsigned int RB;
    unsigned int G;
    unsigned int N;
};

inline void DilateTake(const unsigned char* T, unsigned int Mark, FDilateColumn& S)
{
    // A mask instead of a branch: at a silhouette, which neighbours are visible is
    // anybody's guess, and a wrongly guessed branch costs more than the sum. Written as
    // arithmetic, not as comparisons, so no compiler turns it back into a branch: for a
    // byte V, (V + 255) >> 8 is 1 exactly when V is not zero.
    const unsigned int A    = T[3];
    const unsigned int Take = ((A + 255u) >> 8) & (((A ^ Mark) + 255u) >> 8);
    const unsigned int Mask = 0u - Take;
    S.RB += ((unsigned int)T[0] | ((unsigned int)T[2] << 16)) & Mask;
    S.G  += (unsigned int)T[1] & Mask;
    S.N  += Take;
}

inline void DilateColumnSum(const unsigned char* Ru, const unsigned char* Rc, const unsigned char* Rd,
                            int c, unsigned int Mark, FDilateColumn& S)
{
    S.RB = S.G = S.N = 0;
    DilateTake(Ru + c * 4, Mark, S);
    DilateTake(Rc + c * 4, Mark, S);
    DilateTake(Rd + c * 4, Mark, S);
}

// Sum / Count, rounded down, for a Sum up to 8 * 255 and a Count of 1..8, as a multiply
// and a shift instead of a division: multiplying by 65536 / Count rounded UP gives the
// exact quotient over that whole range (TestDilateEquivalence checks every pair).
inline unsigned int DilateDivide(unsigned int Sum, unsigned int Count)
{
    static const unsigned int Recip[9] = { 0, 65536, 32768, 21846, 16384, 13108, 10923, 9363, 8192 };
    return (Sum * Recip[Count]) >> 16;
}

// One bitmap row ORed with its left and right neighbour: bit x of Out is set if bit x-1, x
// or x+1 of V is, wrapping at both ends. Bits past the row's end stay clear.
inline void DilateHorizontal(const unsigned int* V, unsigned int* Out, int Width, int Words,
                             unsigned int EndMask)
{
    const unsigned int First = V[0] & 1u;
    const unsigned int Last  = (V[Words - 1] >> ((Width - 1) & 31)) & 1u;
    for (int i = 0; i < Words; i++)
    {
        const unsigned int Prev = (i > 0) ? V[i - 1] : 0u;
        const unsigned int Next = (i + 1 < Words) ? V[i + 1] : 0u;
        Out[i] = V[i] | (V[i] << 1) | (Prev >> 31) | (V[i] >> 1) | (Next << 31);
    }
    Out[0]         |= Last;                          // x = 0 sees x = Width-1
    Out[Words - 1] |= First << ((Width - 1) & 31);   // x = Width-1 sees x = 0
    Out[Words - 1] &= EndMask;
}

#if RDX_DILATE_SSE2
// The alphas of 16 texels (64 bytes) side by side in one register: each texel's alpha shifted
// down to its lowest byte, then packed twice - the values are 0..255, so neither pack clips.
inline __m128i DilateAlphas16(const unsigned char* P)
{
    const __m128i A0 = _mm_srli_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(P)), 24);
    const __m128i A1 = _mm_srli_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(P + 16)), 24);
    const __m128i A2 = _mm_srli_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(P + 32)), 24);
    const __m128i A3 = _mm_srli_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(P + 48)), 24);
    return _mm_packus_epi16(_mm_packs_epi32(A0, A1), _mm_packs_epi32(A2, A3));
}

// The visibility bits of 32 texels, as the scalar loop builds them (bit b: texel b's alpha is
// not zero); and into Low, whether any alpha lies in 1..Passes. Alpha - 1 wraps 0 round to 255,
// above every pass count, so "alpha - 1 at most Passes - 1" is exactly that range. Measured on
// a 2048x2048 emblem with its mip chain: the scalar read was 4.3 of the dilation's 8.0 ms.
inline unsigned int DilateVisibleBits32(const unsigned char* P, __m128i PassesLess1, __m128i& Low)
{
    const __m128i Zero = _mm_setzero_si128();
    const __m128i One  = _mm_set1_epi8(1);
    const __m128i Lo   = DilateAlphas16(P);
    const __m128i Hi   = DilateAlphas16(P + 64);
    Low = _mm_or_si128(Low, _mm_cmpeq_epi8(_mm_max_epu8(_mm_sub_epi8(Lo, One), PassesLess1), PassesLess1));
    Low = _mm_or_si128(Low, _mm_cmpeq_epi8(_mm_max_epu8(_mm_sub_epi8(Hi, One), PassesLess1), PassesLess1));
    const unsigned int Transparent =
        (unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(Lo, Zero)) |
        ((unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(Hi, Zero)) << 16);
    return ~Transparent;
}
#endif

// The work memory DilateTransparentEdges needs, in 32-bit words: three bitmaps of one bit per
// texel.
inline long DilateWorkWords(int Width, int Height)
{
    return 3L * (long)((Width + 31) >> 5) * (long)Height;
}

// Work: DilateWorkWords(Width, Height) words from the caller, or null to allocate here. A
// dilation on another thread than the engine's must pass it in: the engine replaces the global
// operator new with its own allocator, which is not thread-safe - and FTextureCache dilates the
// levels of a big texture side by side. Every word is written before it is read.
inline void DilateTransparentEdges(unsigned char* Pixels, int Width, int Height, int Pitch,
                                   int Passes = RDX_DILATE_PASSES, unsigned int* Work = 0)
{
  if (!Pixels || Width <= 0 || Height <= 0 || Passes <= 0)
      return;
  if (Passes > 250)
      Passes = 250;          // a mark must never be mistaken for a visible texel

  // Three bitmaps, one bit per texel: who is visible now (at the start of the current pass),
  // who was before the first pass, and the current one ORed sideways.
  const int  Words = (Width + 31) >> 5;
  const long Total = (long)Words * Height;
  const unsigned int EndMask = (Width & 31) ? (1u << (Width & 31)) - 1u : 0xFFFFFFFFu;
  unsigned int* Vis   = Work ? Work : new unsigned int[Total * 3];
  unsigned int* Start = Vis + Total;
  unsigned int* Horiz = Start + Total;

  // The one full read of the texture. Also notes whether any visible texel has an alpha a
  // mark can take (1..Passes) - see the end. Whole words of 32 texels go through SSE2 where
  // the processor has it; a row's last, partial word texel by texel.
  unsigned int LowAlpha = 0;
#if RDX_DILATE_SSE2
  const __m128i PassesLess1 = _mm_set1_epi8((char)(Passes - 1));
  __m128i       LowVec      = _mm_setzero_si128();
#endif
  for (int y = 0; y < Height; y++)
  {
      const unsigned char* Row = Pixels + (long)y * Pitch;
      unsigned int* V = Vis + (long)y * Words;
      for (int i = 0; i < Words; i++)
      {
          const int x0 = i << 5;
          const int n  = (Width - x0 < 32) ? Width - x0 : 32;
#if RDX_DILATE_SSE2
          if (n == 32)
          {
              V[i] = DilateVisibleBits32(Row + x0 * 4, PassesLess1, LowVec);
              continue;
          }
#endif
          unsigned int Bits = 0;
          for (int b = 0; b < n; b++)
          {
              const unsigned int A = Row[(x0 + b) * 4 + 3];
              Bits     |= (unsigned int)(A != 0) << b;
              LowAlpha |= (unsigned int)(A - 1u < (unsigned int)Passes);
          }
          V[i] = Bits;
      }
  }
#if RDX_DILATE_SSE2
  LowAlpha |= (unsigned int)(_mm_movemask_epi8(LowVec) != 0);
#endif
  for (long i = 0; i < Total; i++)
      Start[i] = Vis[i];

  for (int Pass = 0; Pass < Passes; Pass++)
  {
      const unsigned char Mark = (unsigned char)(Pass + 1);

      // Sideways first, for every row, from the state before this pass: a candidate is a
      // transparent texel whose column is set in the row above, its own or the row below.
      for (int y = 0; y < Height; y++)
          DilateHorizontal(Vis + (long)y * Words, Horiz + (long)y * Words, Width, Words, EndMask);

      // Stop as soon as a pass has nothing left to fill - most textures are opaque or thin.
      int Filled = 0;
      for (int y = 0; y < Height; y++)
      {
          // Wrapped by hand rather than with %, as the sampler wraps.
          const int yu = (y == 0) ? Height - 1 : y - 1;
          const int yd = (y == Height - 1) ? 0 : y + 1;
          const unsigned int* Hu = Horiz + (long)yu * Words;
          const unsigned int* Hc = Horiz + (long)y  * Words;
          const unsigned int* Hd = Horiz + (long)yd * Words;
          const unsigned char* Ru = Pixels + (long)yu * Pitch;
          unsigned char*       Rc = Pixels + (long)y  * Pitch;
          const unsigned char* Rd = Pixels + (long)yd * Pitch;
          unsigned int* V = Vis + (long)y * Words;

          // The columns left of, at and right of the candidate. Only read after a candidate
          // has set them, but the compiler cannot see that.
          FDilateColumn L = { 0, 0, 0 }, C = { 0, 0, 0 }, R = { 0, 0, 0 };
          int Last = -2;              // the previous candidate in this row
          for (int i = 0; i < Words; i++)
          {
              // Computed before this word's fills set their bits.
              unsigned int Cand = (Hu[i] | Hc[i] | Hd[i]) & ~V[i];
              while (Cand)
              {
                  const int b = DilateLowestBit(Cand);
                  Cand &= Cand - 1u;
                  const int x = (i << 5) + b;
                  if (x == Last + 1)
                  {
                      L = C;
                      C = R;
                  }
                  else
                  {
                      DilateColumnSum(Ru, Rc, Rd, (x == 0) ? Width - 1 : x - 1, Mark, L);
                      DilateColumnSum(Ru, Rc, Rd, x, Mark, C);
                  }
                  DilateColumnSum(Ru, Rc, Rd, (x + 1 >= Width) ? x + 1 - Width : x + 1, Mark, R);
                  Last = x;

                  const unsigned int Count = L.N + C.N + R.N;
                  if (Count == 0)
                      continue;   // nothing visible nearby: leave it, nothing samples it either
                  const unsigned int RB = L.RB + C.RB + R.RB;
                  unsigned char* P = Rc + x * 4;
                  P[0] = (unsigned char)DilateDivide(RB & 0xFFFFu, Count);
                  P[1] = (unsigned char)DilateDivide(L.G + C.G + R.G, Count);
                  P[2] = (unsigned char)DilateDivide(RB >> 16, Count);
                  P[3] = Mark;     // filled: only the NEXT pass may spread out from here
                  V[i] |= 1u << b;
                  Filled++;
              }
          }
      }
      if (Filled == 0)
          break;
  }

  // Clearing the marks. They were defined as every alpha 1..Passes, cleared at the
  // end - which also zeroes a VISIBLE texel whose alpha happens to lie in that range. The
  // palette expansion never writes one (it writes 0 and 255), but where there is one, clear
  // exactly as defined; everywhere else the marks are exactly the texels the passes filled.
  if (LowAlpha)
  {
      for (int y = 0; y < Height; y++)
      {
          unsigned char* Row = Pixels + (long)y * Pitch;
          for (int x = 0; x < Width; x++)
              if (Row[x * 4 + 3] != 0 && Row[x * 4 + 3] <= Passes)
                  Row[x * 4 + 3] = 0;
      }
  }
  else
  {
      for (int y = 0; y < Height; y++)
      {
          unsigned char* Row = Pixels + (long)y * Pitch;
          const unsigned int* V = Vis   + (long)y * Words;
          const unsigned int* S = Start + (long)y * Words;
          for (int i = 0; i < Words; i++)
          {
              unsigned int Filled = V[i] & ~S[i];
              while (Filled)
              {
                  Row[((i << 5) + DilateLowestBit(Filled)) * 4 + 3] = 0;
                  Filled &= Filled - 1u;
              }
          }
      }
  }

  if (!Work)
      delete[] Vis;
}

// ---------------------------------------------------------------------------
// Upload geometry
// ---------------------------------------------------------------------------

// D3D12 requires every row of an upload to start on a 256-byte boundary.
inline int AlignRowPitch(int Bytes)
{
    const int Alignment = 256;
    return (Bytes + Alignment - 1) & ~(Alignment - 1);
}

// Block-compressed formats store 4x4 blocks; a "row" is a row of blocks.
inline int Bc1RowBytes(int Width)  { return ((Width + 3) / 4) * 8; }
inline int Bc1RowCount(int Height) { return (Height + 3) / 4; }

// A convex polygon of N points becomes N-2 triangles as a fan.
inline int FanVertexCount(int Points)
{
    return (Points >= 3) ? (Points - 2) * 3 : 0;
}

// ---------------------------------------------------------------------------
// Caption - a short text of the renderer's own on the screen (FFinalPass::DrawCaption)
// ---------------------------------------------------------------------------

// A 5x7 dot font of our own, drawn for this project: upper-case letters (lower case comes out upper case), digits and
// : . , - / % ( ) = +. Row by row from the top, '1' a lit dot, the left column first.
struct FCaptionGlyph
{
    char        C;
    const char* Rows[7];
};

inline const FCaptionGlyph* CaptionFont(int& Count)
{
    static const FCaptionGlyph Font[] =
    {
        { 'A', { "01110", "10001", "10001", "11111", "10001", "10001", "10001" } },
        { 'B', { "11110", "10001", "10001", "11110", "10001", "10001", "11110" } },
        { 'C', { "01110", "10001", "10000", "10000", "10000", "10001", "01110" } },
        { 'D', { "11110", "10001", "10001", "10001", "10001", "10001", "11110" } },
        { 'E', { "11111", "10000", "10000", "11110", "10000", "10000", "11111" } },
        { 'F', { "11111", "10000", "10000", "11110", "10000", "10000", "10000" } },
        { 'G', { "01110", "10001", "10000", "10111", "10001", "10001", "01111" } },
        { 'H', { "10001", "10001", "10001", "11111", "10001", "10001", "10001" } },
        { 'I', { "01110", "00100", "00100", "00100", "00100", "00100", "01110" } },
        { 'J', { "00111", "00010", "00010", "00010", "00010", "10010", "01100" } },
        { 'K', { "10001", "10010", "10100", "11000", "10100", "10010", "10001" } },
        { 'L', { "10000", "10000", "10000", "10000", "10000", "10000", "11111" } },
        { 'M', { "10001", "11011", "10101", "10101", "10001", "10001", "10001" } },
        { 'N', { "10001", "10001", "11001", "10101", "10011", "10001", "10001" } },
        { 'O', { "01110", "10001", "10001", "10001", "10001", "10001", "01110" } },
        { 'P', { "11110", "10001", "10001", "11110", "10000", "10000", "10000" } },
        { 'Q', { "01110", "10001", "10001", "10001", "10101", "10010", "01101" } },
        { 'R', { "11110", "10001", "10001", "11110", "10100", "10010", "10001" } },
        { 'S', { "01111", "10000", "10000", "01110", "00001", "00001", "11110" } },
        { 'T', { "11111", "00100", "00100", "00100", "00100", "00100", "00100" } },
        { 'U', { "10001", "10001", "10001", "10001", "10001", "10001", "01110" } },
        { 'V', { "10001", "10001", "10001", "10001", "10001", "01010", "00100" } },
        { 'W', { "10001", "10001", "10001", "10101", "10101", "10101", "01010" } },
        { 'X', { "10001", "10001", "01010", "00100", "01010", "10001", "10001" } },
        { 'Y', { "10001", "10001", "10001", "01010", "00100", "00100", "00100" } },
        { 'Z', { "11111", "00001", "00010", "00100", "01000", "10000", "11111" } },
        { '0', { "01110", "10001", "10011", "10101", "11001", "10001", "01110" } },
        { '1', { "00100", "01100", "00100", "00100", "00100", "00100", "01110" } },
        { '2', { "01110", "10001", "00001", "00010", "00100", "01000", "11111" } },
        { '3', { "11111", "00010", "00100", "00010", "00001", "10001", "01110" } },
        { '4', { "00010", "00110", "01010", "10010", "11111", "00010", "00010" } },
        { '5', { "11111", "10000", "11110", "00001", "00001", "10001", "01110" } },
        { '6', { "00110", "01000", "10000", "11110", "10001", "10001", "01110" } },
        { '7', { "11111", "00001", "00010", "00100", "01000", "01000", "01000" } },
        { '8', { "01110", "10001", "10001", "01110", "10001", "10001", "01110" } },
        { '9', { "01110", "10001", "10001", "01111", "00001", "00010", "01100" } },
        { ':', { "00000", "01100", "01100", "00000", "01100", "01100", "00000" } },
        { '.', { "00000", "00000", "00000", "00000", "00000", "01100", "01100" } },
        // The readout's frame rate lines are split with it ("SCREEN 60 HZ, VRR ON").
        { ',', { "00000", "00000", "00000", "00000", "01100", "00100", "01000" } },
        { '-', { "00000", "00000", "00000", "11111", "00000", "00000", "00000" } },
        { '/', { "00000", "00001", "00010", "00100", "01000", "10000", "00000" } },
        { '%', { "11000", "11001", "00010", "00100", "01000", "10011", "00011" } },
        { '(', { "00010", "00100", "01000", "01000", "01000", "00100", "00010" } },
        { ')', { "01000", "00100", "00010", "00010", "00010", "00100", "01000" } },
        { '=', { "00000", "00000", "11111", "00000", "11111", "00000", "00000" } },
        // A menu's selection marker needs one of these, and a caption that says "press right" wants the other.
        { '>', { "10000", "01000", "00100", "00010", "00100", "01000", "10000" } },
        { '<', { "00001", "00010", "00100", "01000", "00100", "00010", "00001" } },
        { '+', { "00000", "00100", "00100", "11111", "00100", "00100", "00000" } },
    };
    Count = (int)(sizeof(Font) / sizeof(Font[0]));
    return Font;
}

// The dots of one character as the caption shader reads them: bit Row * 5 + Column - rows 0-3 in Lo (bits 0-19), rows 4-6
// in Hi (bits 0-14). False, with no dot, for a character the font does not have: it is drawn as a space.
inline bool CaptionGlyph(char C, unsigned& Lo, unsigned& Hi)
{
    if (C >= 'a' && C <= 'z')
        C = (char)(C - 'a' + 'A');
    Lo = Hi = 0;
    int Count = 0;
    const FCaptionGlyph* Font = CaptionFont(Count);
    for (int g = 0; g < Count; g++)
    {
        if (Font[g].C != C)
            continue;
        for (int Row = 0; Row < 7; Row++)
            for (int Col = 0; Col < 5; Col++)
                if (Font[g].Rows[Row][Col] == '1')
                {
                    const int Bit = Row * 5 + Col;
                    if (Bit < 20)
                        Lo |= 1u << Bit;
                    else
                        Hi |= 1u << (Bit - 20);
                }
        return true;
    }
    return false;
}

// Every glyph seven rows of five '0' or '1', and no character twice - for the tests.
inline bool CaptionFontWellFormed()
{
    int Count = 0;
    const FCaptionGlyph* Font = CaptionFont(Count);
    for (int g = 0; g < Count; g++)
    {
        for (int h = g + 1; h < Count; h++)
            if (Font[h].C == Font[g].C)
                return false;
        for (int Row = 0; Row < 7; Row++)
        {
            const char* R = Font[g].Rows[Row];
            for (int Col = 0; Col < 5; Col++)
                if (R[Col] != '0' && R[Col] != '1')
                    return false;
            if (R[5] != 0)
                return false;
        }
    }
    return true;
}

// The caption's size in pixels: a dot of margin left, top and bottom, six dots a character (the sixth a gap - after the
// last one, the right margin), Scale pixels a dot. The scale from the output's height: 4 at 4K, 2 at 1080p, at least
// 1 - one font pixel to this many screen pixels. The font is our own, not the game's UI, so nothing else moves with it.
inline int CaptionScale(int OutputHeight)          { const int S = (OutputHeight + 250) / 500; return (S < 1) ? 1 : S; }
// The same, from the output's SHORTER side: on a monitor turned upright the height would give a caption wider than the
// screen (1080x1920 would scale by 4, and a 48-character line would be 1084 pixels across).
inline int CaptionScale(int OutputWidth, int OutputHeight)
{
    return CaptionScale((OutputWidth < OutputHeight) ? OutputWidth : OutputHeight);
}
inline int CaptionWidth(int Characters, int Scale) { return (1 + Characters * 6) * Scale; }
inline int CaptionHeight(int Scale)                { return 9 * Scale; }
// The scale a block of so many lines is drawn at, its top at TopY on a picture so high: the picture's own (Scale) where
// the block fits above the picture's foot, a size smaller where it does not, down to one. A line past the foot is not
// drawn at all - on a low screen that would be the menu's last lines, which the selection could still step onto.
inline int CaptionScaleToFit(int Lines, int TopY, int OutputHeight, int Scale)
{
    while (Scale > 1 && TopY + Lines * CaptionHeight(Scale) > OutputHeight)
        Scale--;
    return Scale;
}

// A root signature holds at most 64 32-bit units: with 68 root constants (4 + 2 x 32 characters) CreateRootSignature
// refused the caption's (D3D12 ERROR #699 on the validation layer). So a caption has at most 30
// characters - 4 + 2 x 30 = 64 - and the caption shader's G array holds 15 x 4 values.
enum { CaptionMaxChars = 30, CaptionConstants = 4 + 2 * CaptionMaxChars };
// ... and that is a limit on ONE DRAW, not on a line: a longer line is drawn in pieces of CaptionMaxChars, side by side
// (FBackend::DrawCaption). This is the line's own limit - wide enough for a menu value of thirty characters beside its
// label, which the preset's longest name needs.
enum { CaptionLineChars = 48 };

// A caption may be several lines, one under the other, each its own draw with its own box (FBackend::DrawCaption): the
// 30 characters above are a limit on THE LINE, and the readout's longest lines use all 30 of them.
//
// How many lines a block may have - the menu on the left and the readout on the right each fill one on their own. The
// menu is the longer: a title, every item, and the blank lines between its groups (the static_asserts at BuildReadout,
// RenderDevice.cpp, count them). A line past the last is not cut short, it is simply never drawn, so this is kept with
// room to spare.
enum { CaptionLines = 50 };

// The process's own address space, where the readout warns of it: with fewer megabytes of it free than this, a 32-bit
// game is near the allocation it cannot make - the engine ends there with "Ran out of virtual memory". A level's
// textures come in pieces of up to a few megabytes each, and the free space is cut up besides, so the limit stands
// well above nought. Below nought: the system gave no number, and nothing is said.
enum { AddressSpaceLowMB = 500 };
inline bool AddressSpaceLow(int FreeMB)
{
    return FreeMB >= 0 && FreeMB < AddressSpaceLowMB;
}

// Where the caption stands: in the top right-hand corner, clear of the game's own column of augmentation icons down the
// right edge. The game scales its interface with the screen's height; the column was measured at 3840x2160, 178 pixels
// from the right edge - a little over 8% of the height - so 9% of it leaves a gap. From the top, the caption's own
// distance: a fortieth of the height.
const float CaptionRightGap = 0.09f;
// And where the renderer's menu stands: the top LEFT corner, clear of the game's health figure and the bar under
// it. That block was measured at 3840x2160 as well, 304 pixels wide - a shade over 14% of the height - so 16% of
// it leaves a gap. The two blocks are then on screen together, the menu on one side and the readout on the other.
const float CaptionLeftGap  = 0.16f;
const float CaptionTopGap   = 0.025f;

// ---------------------------------------------------------------------------
// The flashlight (FlashlightBeam)
// ---------------------------------------------------------------------------
//
// The game's own flashlight (DeusEx.AugLight) is a ball of light: a light set down 64 units short of whatever the
// view's ray meets, 125 to 250 units in reach. It lights a patch round that point, and as the point jumps from a near
// wall's edge to a far wall, so does the light. In its place a cone of light from the eye along the view: every pixel
// of the world's view is lit by where it lies in the cone and how far off it is, so nothing jumps. The light and the
// eye are in one place - what the light cannot reach, the eye cannot see - so it needs no shadows.
//
// BeamReach: as far as the game's own reaches (AugLight's LevelValues - the ray it traces). The light is at its
// strongest in the hot spot round the view's middle, within BeamHotCos of the axis (cosines, all four), and falls to
// nothing at BeamRimCos; round that lies a weaker spill, BeamSpillShare of it, full within BeamSpillHotCos and gone
// at BeamSpillRimCos - a torch's reflector, and what escapes it. Along the ray it falls to half at BeamHalfDist and
// to nothing over the reach's second half; it begins BeamNearStart from the eye and is full by BeamNearFull, so what
// is held in the hand stays out of it. A surface turned away from the eye takes less of it, BeamFacingFloor at the
// least: a wall seen along its length is still lit, as it is by the game's own, which asks no angle. The reach is long
// and its fade over its second half, as a torch's light thins out on a lawn: cut over its last quarter at 1024 units,
// it went from plain to nothing within a few metres.
static const float BeamReach       = 4096.0f;
static const float BeamHotCos      = 0.9903f;
static const float BeamRimCos      = 0.9455f;
static const float BeamSpillHotCos = 0.9063f;
static const float BeamSpillRimCos = 0.7880f;
static const float BeamSpillShare  = 0.22f;
static const float BeamHalfDist    = 384.0f;
static const float BeamNearStart   = 20.0f;
static const float BeamNearFull    = 56.0f;
static const float BeamFacingFloor = 0.35f;
// The cone's light at its strongest is what the engine makes of the game's own light's hue, saturation and brightness,
// times BeamGain: the game's is a weak light close to its wall, the cone a strong one that thins out along its reach.
// BeamEyeSlack: how far from the player's eyes the world's view may have been drawn, the frame before, for the cone
// to stand in the game's lights' place - a lean to the side and a frame's run fit in it, a camera set elsewhere not.
static const float BeamGain        = 2.0f;
static const float BeamEyeSlack    = 96.0f;
// A see-through surface is laid over what is behind it, so the light on it is light added to the picture: it takes
// BeamSeeThrough of the cone - with BeamGain, what the game's own light would give it. All of the cone turns a pane
// or a hologram into a white haze; none of it, and a pane in the dark that the game's flashlight shows is not seen.
static const float BeamSeeThrough  = 0.5f;
// From 0 at Lo to 1 at Hi, with no corner at either end.
inline float BeamStep(float Lo, float Hi, float V)
{
    float T = (V - Lo) / (Hi - Lo);
    T = (T < 0.0f) ? 0.0f : ((T > 1.0f) ? 1.0f : T);
    return T * T * (3.0f - 2.0f * T);
}
// How much of the flashlight's light a point takes, 0..1: Dist from the eye, Cos the cosine of its angle from the
// view's axis, Facing the cosine between its surface's normal and the direction to the eye. The shader's own
// arithmetic (SolidPass.hlsl, BeamLight) is this, number for number.
inline float BeamLight(float Dist, float Cos, float Facing)
{
    if (!(Dist > BeamNearStart) || !(Dist < BeamReach))
        return 0.0f;
    const float Hot   = BeamStep(BeamRimCos, BeamHotCos, Cos);
    const float Spill = BeamStep(BeamSpillRimCos, BeamSpillHotCos, Cos);
    const float Cone  = Hot + (1.0f - Hot) * BeamSpillShare * Spill;
    const float Near  = BeamStep(BeamNearStart, BeamNearFull, Dist);
    const float Far   = 1.0f - BeamStep(0.5f * BeamReach, BeamReach, Dist);
    const float Fall  = 1.0f / (1.0f + (Dist / BeamHalfDist) * (Dist / BeamHalfDist));
    const float Faces = (Facing < 0.0f) ? 0.0f : ((Facing > 1.0f) ? 1.0f : Facing);
    return Cone * Near * Far * Fall * (BeamFacingFloor + (1.0f - BeamFacingFloor) * Faces);
}
// The block the shader reads the beam from (BeamConsts), sixteen numbers: Colour is its light at its strongest; the
// view's centre and projection are in the target's own pixels. With no colour (null) the block says there is no beam.
enum { BeamBlockFloats = 16 };
inline void BeamBlock(const float* Colour, float CentreX, float CentreY, float Focal, float* Out)
{
    for (int i = 0; i < BeamBlockFloats; i++)
        Out[i] = 0.0f;
    if (!Colour)
        return;
    Out[0]  = Colour[0];
    Out[1]  = Colour[1];
    Out[2]  = Colour[2];
    Out[3]  = BeamReach;
    Out[4]  = BeamRimCos;
    Out[5]  = 1.0f / (BeamHotCos - BeamRimCos);
    Out[6]  = BeamSpillRimCos;
    Out[7]  = 1.0f / (BeamSpillHotCos - BeamSpillRimCos);
    Out[8]  = BeamHalfDist;
    Out[9]  = BeamNearStart;
    Out[10] = 1.0f / (BeamNearFull - BeamNearStart);
    Out[11] = BeamSpillShare;
    Out[12] = CentreX;
    Out[13] = CentreY;
    Out[14] = Focal;
    Out[15] = BeamFacingFloor;
}

// The widest disc, in the mirrors' map's pixels, a surface looks the map up over (SolidPass.hlsl: BeamOffMirrors holds
// its disc to this). And how far from the view's axis any place of the map is looked up at all, as a cosine: the
// spill's rim, and that disc beyond it with a pixel to spare each side - a place further out no surface reads, so
// nothing need be put there. Focal: the view's projection in the map's pixels; 0 where it is not known.
const float BeamDiscMost = 48.0f;
inline float BeamMapRimCos(float Focal)
{
    if (!(Focal > 1.0f))
        return 0.0f;
    const float Rim = Sqrt(1.0f - BeamSpillRimCos * BeamSpillRimCos) / BeamSpillRimCos + (BeamDiscMost + 2.0f) / Focal;
    return 1.0f / Sqrt(1.0f + Rim * Rim);
}

// Flashlight: Original - the game's own flashlight everywhere; Fixed - the cone from the eye in its place (above);
// Everybody - Fixed, and besides it a lamp on the gun of each of the game's people who holds one up at somebody (the
// weapon lamps, RenderDevice.cpp's GatherWeaponLamps). No preset holds Everybody.
enum EFlashlight
{
    Flashlight_Original = 0,
    Flashlight_Fixed,
    Flashlight_Everybody,
    Flashlight_Count
};

inline const char* const* FlashlightNames()
{
    static const char* const Names[Flashlight_Count] = { "Original", "Fixed", "Everybody" };
    return Names;
}

template <typename TChar>
inline void FlashlightName(EFlashlight F, TChar* Out, int Max)
{
    const char* const N = FlashlightNames()[(F >= 0 && F < Flashlight_Count) ? (int)F : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Fixed - the default - for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline EFlashlight ParseFlashlight(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return Flashlight_Fixed;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    for (int i = 0; i < Flashlight_Count; i++)
    {
        const char* const N = FlashlightNames()[i];
        int k = 0;
        while (N[k] && Text[B + k])
        {
            const TChar a  = Text[B + k];
            const char  b  = N[k];
            const TChar la = (a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a;
            const char  lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != (TChar)lb)
                break;
            k++;
        }
        if (!N[k] && (!Text[B + k] || Text[B + k] == ' ' || Text[B + k] == '\t' || Text[B + k] == '\r'))
        {
            bKnown = true;
            return (EFlashlight)i;
        }
    }
    return Flashlight_Fixed;
}

inline EFlashlight FlashlightStep(EFlashlight F, int Step)
{
    int N = (int)F + Step;
    while (N < 0)
        N += Flashlight_Count;
    return (EFlashlight)(N % Flashlight_Count);
}

// The own lamps: the weapon lamps (Flashlight=Everybody), and the flashlight's cone itself where a view is not the
// eye's - a mirror's view of the level, and what the mirror rays meet. Each is a spot in the level with a map of
// depths drawn from it (FShadowMaps::RenderSpots): unlike the eye's own cone, such a lamp lights places the eye sees
// and the lamp may not, so whatever stands in its way - the level or anybody - keeps its light off.
// The torch is the flashlight's (Beam*): its hot spot and spill, its fall along the way, its near fade. Its reach is
// OwnLampReach, the map's: the level goes into the map as far as that, so nothing past it may be lit - the light fades
// out over the reach's second half. The map sees OwnLampMapTan to either side at unit distance - the spill's rim with
// room to spare - in OwnLampMapSize texels a side, its depth OwnLampNear over the distance along the axis, as the cube
// faces' (CubeFaceMatrix). OwnLampMost: the lamps a frame takes, the flashlight's among them, the weapon lamps
// nearest the eye first - in a fight, the far ones are the ones left out.
static const float OwnLampReach   = 2048.0f;
static const float OwnLampMapTan  = 0.84f;
static const float OwnLampNear    = 4.0f;
enum { OwnLampMapSize = 1024, OwnLampMost = 9 };
// A weapon lamp's light at its strongest, as the flashlight's cone has its own (BeamGain on the game's light): a
// torch's white, a little cool.
static const float WeaponLampLight[3] = { 1.30f, 1.38f, 1.45f };

// How much of an own lamp's light reaches a place Dist from it, Cos off its axis, on a face whose normal turns Facing
// (a cosine) towards the lamp, 0..1 - SolidPass.hlsl's OwnLampsAt is the same arithmetic. A face turned away takes
// none of it; one the light runs along still takes BeamFacingFloor, as the cone from the eye has it, reached over the
// first tenth so that nothing jumps where a face turns away.
inline float OwnLampLight(float Dist, float Cos, float Facing)
{
    if (!(Dist > BeamNearStart) || !(Dist < OwnLampReach) || !(Facing > 0.0f))
        return 0.0f;
    const float Hot   = BeamStep(BeamRimCos, BeamHotCos, Cos);
    const float Spill = BeamStep(BeamSpillRimCos, BeamSpillHotCos, Cos);
    const float Cone  = Hot + (1.0f - Hot) * BeamSpillShare * Spill;
    const float Near  = BeamStep(BeamNearStart, BeamNearFull, Dist);
    const float Far   = 1.0f - BeamStep(0.5f * OwnLampReach, OwnLampReach, Dist);
    const float Fall  = 1.0f / (1.0f + (Dist / BeamHalfDist) * (Dist / BeamHalfDist));
    const float F     = (Facing > 1.0f) ? 1.0f : Facing;
    const float Turn  = (F < 0.1f) ? F * 10.0f : 1.0f;
    return Cone * Near * Far * Fall * Turn * (BeamFacingFloor + (1.0f - BeamFacingFloor) * F);
}

// How strongly a lamp shines along a direction Cos off its axis, 0..1: its cone alone - the lamp as the eye sees it
// when the eye is in its light (the disc, its glare and the lens's ghosts), as the lamp's image in a mirror has it.
inline float OwnLampToward(float Cos)
{
    const float Hot   = BeamStep(BeamRimCos, BeamHotCos, Cos);
    const float Spill = BeamStep(BeamSpillRimCos, BeamSpillHotCos, Cos);
    return Hot + (1.0f - Hot) * BeamSpillShare * Spill;
}

// The own lamps' rows as the passes read them (SolidPass.hlsl's OwnRow; the walk's OwnBeams), in Rows, which the
// caller has zeroed: the head - how many lamps; a map's texel at unit distance, the maps' near distance -, the torch's
// rims, its fall and near fade, its reach, its facing floor and a see-through surface's share (OwnLampRowsHead); then
// each lamp's seven - where it is, its axis and its map's slice (its number), its colour and 1 where it is the
// flashlight's own, the eye's lamp, its map's four rows (SpotMatrix) (OwnLampRowsLamp).
inline void OwnLampRowsHead(float* Rows, int Count)
{
    Rows[0]  = (float)Count;
    Rows[1]  = 2.0f * OwnLampMapTan / (float)OwnLampMapSize;
    Rows[2]  = OwnLampNear;
    Rows[4]  = BeamRimCos;
    Rows[5]  = 1.0f / (BeamHotCos - BeamRimCos);
    Rows[6]  = BeamSpillRimCos;
    Rows[7]  = 1.0f / (BeamSpillHotCos - BeamSpillRimCos);
    Rows[8]  = BeamHalfDist;
    Rows[9]  = BeamNearStart;
    Rows[10] = 1.0f / (BeamNearFull - BeamNearStart);
    Rows[11] = BeamSpillShare;
    Rows[12] = OwnLampReach;
    Rows[13] = BeamFacingFloor;
    Rows[14] = BeamSeeThrough;
}
inline void OwnLampRowsLamp(float* Rows, int Index, const FVec3& At, const FVec3& Aim, const float* Colour, bool bEye,
                            const float* Map)
{
    float* const L = Rows + 4 * (RDX_OWN_HEAD_ROWS + Index * RDX_OWN_LAMP_ROWS);
    L[0]  = At.X;
    L[1]  = At.Y;
    L[2]  = At.Z;
    L[4]  = Aim.X;
    L[5]  = Aim.Y;
    L[6]  = Aim.Z;
    L[7]  = (float)Index;
    L[8]  = Colour[0];
    L[9]  = Colour[1];
    L[10] = Colour[2];
    L[11] = bEye ? 1.0f : 0.0f;
    for (int i = 0; i < 16; i++)
        L[12 + i] = Map[i];
}

// The air in a weapon lamp's cone, as a dusty room shows a torch's beam (the walk's OwnBeams): how much of the light
// that crosses a unit of it is scattered, by the light the cone puts on the level (OwnLampLight, with no face), and
// how far the scattering leans forwards (OwnBeamForward, Henyey-Greenstein's g) - brightest looking into the lamp,
// fainter from the side. A step of the walk about every OwnBeamStepWorld units of the ray's stretch in the cone, no
// more than OwnBeamStepsMost of them.
static const float OwnBeamDust      = 3.0e-4f;
static const float OwnBeamForward   = 0.3f;
static const float OwnBeamStepWorld = 24.0f;
enum { OwnBeamStepsMost = 32 };

// The stretch of a ray in a lamp's cone and within its reach (the walk's OwnBeamSpan is the same arithmetic): the
// ray's places From + V * z for z from Z0 to Z1 - From its start less the lamp's place, V its step, of any length -,
// the cone round the unit Axis out to the cosine Cos (above 0), on the lamp's own side: the equation of a cone holds
// a second one, the other way from its tip. False where none of the ray is in it; the stretch in Out0, Out1 otherwise.
inline bool OwnLampSpan(const FVec3& From, const FVec3& V, const FVec3& Axis, float Cos, float Reach, float Z0,
                        float Z1, float& Out0, float& Out1)
{
    const float VV = Dot(V, V), FV = Dot(From, V), FF = Dot(From, From);
    const float VA = Dot(V, Axis), FA = Dot(From, Axis);
    if (!(VV > 1e-12f))
        return false;
    float      Lo    = Z0, Hi = Z1;
    const auto Above = [&](float Z) { if (Z > Lo) Lo = Z; };
    const auto Below = [&](float Z) { if (Z < Hi) Hi = Z; };
    // Within reach: VV z^2 + 2 FV z + FF no more than Reach^2.
    const float Ball = FV * FV - VV * (FF - Reach * Reach);
    if (!(Ball > 0.0f))
        return false;
    const float BallRoot = Sqrt(Ball);
    Above((-FV - BallRoot) / VV);
    Below((-FV + BallRoot) / VV);
    // On the lamp's side of it: FA + VA z at least 0.
    if (VA > 0.0f)
        Above(-FA / VA);
    else if (VA < 0.0f)
        Below(-FA / VA);
    else if (FA < 0.0f)
        return false;
    // In the cone there: (FA + VA z)^2 at least Cos^2 times the distance squared - A z^2 + B z + C at least 0. Where A
    // is below 0 the ray crosses the cone, in between the two roots; above 0 it runs within the cone's angle, and is
    // in it from the root on the lamp's side on; at 0 it runs along the cone's side.
    const float C2   = Cos * Cos;
    const float A    = VA * VA - C2 * VV;
    const float B    = 2.0f * (VA * FA - C2 * FV);
    const float C    = FA * FA - C2 * FF;
    const float Disc = B * B - 4.0f * A * C;
    if (A < -1e-6f * VV)
    {
        if (!(Disc > 0.0f))
            return false;
        const float R = Sqrt(Disc);
        Above((-B + R) / (2.0f * A));
        Below((-B - R) / (2.0f * A));
    }
    else if (A > 1e-6f * VV)
    {
        if (Disc > 0.0f)
        {
            const float R = Sqrt(Disc);
            if (VA > 0.0f)
                Above((-B + R) / (2.0f * A));
            else
                Below((-B - R) / (2.0f * A));
        }
    }
    else if (B > 0.0f)
        Above(-C / B);
    else if (B < 0.0f)
        Below(-C / B);
    else if (C < 0.0f)
        return false;
    if (!(Hi > Lo))
        return false;
    Out0 = Lo;
    Out1 = Hi;
    return true;
}

// A spot's map (FShadowMaps::RenderSpots, SolidPass.hlsl's OwnShadow): the four rows that take a place in the level
// into the map's clip space - x right and y up over Tan at unit distance, z Near and w the distance along the axis,
// so the depth is Near over that distance, as the cube faces' (CubeFaceMatrix). Axis: a unit vector; right and up are
// made square to it from the level's up, or from X where the axis lies along the level's up.
inline void SpotMatrix(const FVec3& At, const FVec3& Axis, float Tan, float Near, float M[16])
{
    const bool  bUpright = Axis.Z > 0.99f || Axis.Z < -0.99f;
    const FVec3 Up       = { bUpright ? 1.0f : 0.0f, 0.0f, bUpright ? 0.0f : 1.0f };
    FVec3 R = { Up.Y * Axis.Z - Up.Z * Axis.Y, Up.Z * Axis.X - Up.X * Axis.Z, Up.X * Axis.Y - Up.Y * Axis.X };
    const float RL = Sqrt(Dot(R, R));
    R.X /= RL;
    R.Y /= RL;
    R.Z /= RL;
    const FVec3 U = { Axis.Y * R.Z - Axis.Z * R.Y, Axis.Z * R.X - Axis.X * R.Z, Axis.X * R.Y - Axis.Y * R.X };
    const float S = 1.0f / Tan;
    M[0]  = R.X * S;  M[1]  = R.Y * S;  M[2]  = R.Z * S;  M[3]  = -Dot(R, At) * S;
    M[4]  = U.X * S;  M[5]  = U.Y * S;  M[6]  = U.Z * S;  M[7]  = -Dot(U, At) * S;
    M[8]  = 0.0f;     M[9]  = 0.0f;     M[10] = 0.0f;     M[11] = Near;
    M[12] = Axis.X;   M[13] = Axis.Y;   M[14] = Axis.Z;   M[15] = -Dot(Axis, At);
}

// Whether a sphere (Centre, Radius) may reach into a spot's light: within Reach of the lamp, and not wholly outside
// the cone whose half angle has the cosine Cos and the sine Sin - conservative, as the cube faces' test is.
inline bool SphereTouchesSpot(const FVec3& At, const FVec3& Axis, float Cos, float Sin, float Reach, const FVec3& Centre,
                              float Radius)
{
    const FVec3 V    = { Centre.X - At.X, Centre.Y - At.Y, Centre.Z - At.Z };
    const float D2   = Dot(V, V);
    const float Far  = Reach + Radius;
    if (D2 > Far * Far)
        return false;
    if (D2 <= Radius * Radius)
        return true;
    const float Along = Dot(V, Axis);
    const float Off2  = D2 - Along * Along;
    const float Off   = Sqrt(Off2 > 0.0f ? Off2 : 0.0f);
    // The distance from the cone's surface, outward positive: the centre's place turned into the cone's own plane.
    return Off * Cos - Along * Sin <= Radius;
}

// A mirror's view in the level (the own lamps in a mirror's view of the level): the view's surfaces are rebuilt from
// their pixels with the world's view's numbers, and so come out behind the mirror - each place's image; the place is
// that image mirrored by the mirror's plane. With the world's view's origin and axes in the level (camera to level)
// and the mirror's plane in that view's space (its unit normal away from the eye, then its distance), the origin and
// axes that take a place rebuilt as P to the level, at Om + P.X * Xm + P.Y * Ym + P.Z * Zm.
inline void MirrorViewCoords(const FVec3& O, const FVec3& X, const FVec3& Y, const FVec3& Z, const float* Plane,
                             FVec3& Om, FVec3& Xm, FVec3& Ym, FVec3& Zm)
{
    const FVec3 N = { Plane[0] * X.X + Plane[1] * Y.X + Plane[2] * Z.X, Plane[0] * X.Y + Plane[1] * Y.Y + Plane[2] * Z.Y,
                      Plane[0] * X.Z + Plane[1] * Y.Z + Plane[2] * Z.Z };
    const float W = 2.0f * Plane[3];
    Om.X = O.X + N.X * W;
    Om.Y = O.Y + N.Y * W;
    Om.Z = O.Z + N.Z * W;
    const float Ax = 2.0f * Dot(N, X), Ay = 2.0f * Dot(N, Y), Az = 2.0f * Dot(N, Z);
    Xm.X = X.X - N.X * Ax;  Xm.Y = X.Y - N.Y * Ax;  Xm.Z = X.Z - N.Z * Ax;
    Ym.X = Y.X - N.X * Ay;  Ym.Y = Y.Y - N.Y * Ay;  Ym.Z = Y.Z - N.Z * Ay;
    Zm.X = Z.X - N.X * Az;  Zm.Y = Z.Y - N.Y * Az;  Zm.Z = Z.Z - N.Z * Az;
}

// Whether a character's animation holds the gun up at what it shoots at (GatherWeaponLamps): the game's shooting and
// strafing animations - Shoot, Shoot2H, CrouchShoot, RunShoot, TreadShoot, Strafe2H and the rest of their kind, in
// DeusExCharacters, HDTPCharacters and MPCharacters alike; not the gun carried across the chest (BreatheLight2H,
// Walk2H), not a reload, not a blow by hand.
template <typename TChar>
inline bool WeaponLampAims(const TChar* Anim)
{
    if (!Anim)
        return false;
    static const char Shoot[]  = "shoot";
    static const char Strafe[] = "strafe";
    bool bStrafe = true;
    for (int k = 0; Strafe[k]; k++)
    {
        const TChar a = Anim[k];
        if (!a || ((a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a) != (TChar)Strafe[k])
        {
            bStrafe = false;
            break;
        }
    }
    if (bStrafe)
        return true;
    for (int i = 0; Anim[i]; i++)
    {
        int k = 0;
        for (; Shoot[k]; k++)
        {
            const TChar a = Anim[i + k];
            if (!a || ((a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a) != (TChar)Shoot[k])
                break;
        }
        if (!Shoot[k])
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Shadow maps (docs/terv-latvany-roadmap.md 3.2)
// ---------------------------------------------------------------------------
//
// Everything in view casts a shadow, the player included, from the nearest 5-10 lights - those within X metres of
// the player in the direction of view, reselected as the player moves. X comes from a preset: Off / Low / Medium / High in
// the ini, Off being the vanilla behaviour. Each chosen light gets two cube maps: one of the level's own geometry (drawn when
// the light is chosen) and one of the actors (drawn every frame).

// More mirrors of the engine's EPolyFlags (Engine\Inc\UnObj.h:221-273), for the occluder rules below.
enum EShadowPolyFlagBits
{
    RDX_PF_Invisible = 0x00000001,
    RDX_PF_Portal    = 0x04000000,
};

enum EShadowQuality
{
    ShadowQuality_Off = 0,
    ShadowQuality_Low,
    ShadowQuality_Medium,
    ShadowQuality_High,
    // High's lights and casters, the shadows traced with rays instead of drawn into maps (docs/terv-rt.md) - on a card
    // that traces rays; elsewhere High itself (the readout says HIGH(FALLBACK)). RT High; the ini's "RT" is this one.
    ShadowQuality_Rt,
    // The same with Low's and with Medium's lights, and Low and Medium where rays cannot run (ShadowMapQualityFor).
    // After RT so that the numbers the ini already knows keep their meaning; the menu's order is ShadowQualityStep's.
    ShadowQuality_RtLow,
    ShadowQuality_RtMedium,
    ShadowQuality_Count
};

// Whether a quality traces rays.
inline bool IsRtShadowQuality(EShadowQuality Q)
{
    return Q == ShadowQuality_Rt || Q == ShadowQuality_RtLow || Q == ShadowQuality_RtMedium;
}

// The maps' quality whose lights a quality takes - the ray traced ones follow Low, Medium and High light for light, the
// graduated pick included - and which runs in its place where rays cannot.
inline EShadowQuality ShadowMapQualityFor(EShadowQuality Q)
{
    switch (Q)
    {
    case ShadowQuality_RtLow:    return ShadowQuality_Low;
    case ShadowQuality_RtMedium: return ShadowQuality_Medium;
    case ShadowQuality_Rt:       return ShadowQuality_High;
    default:                     return Q;
    }
}

// The menu's step: Off, Low, Medium, High, RT Low, RT Medium, RT High, round again - Step places along (-1 or 1).
inline EShadowQuality ShadowQualityStep(EShadowQuality Q, int Step)
{
    static const EShadowQuality Order[7] = { ShadowQuality_Off, ShadowQuality_Low, ShadowQuality_Medium, ShadowQuality_High,
                                             ShadowQuality_RtLow, ShadowQuality_RtMedium, ShadowQuality_Rt };
    int At = 0;
    for (int k = 0; k < 7; k++)
        At = (Order[k] == Q) ? k : At;
    return Order[((At + Step) % 7 + 7) % 7];
}

// The most lights a preset may ask for: RDX_SHADOW_MAX_LIGHTS, in RenderLimits.h (included at the top).
#define RDX_STRINGIFY2(x) #x
#define RDX_STRINGIFY(x)  RDX_STRINGIFY2(x)
// The casters have no number of this kind: the lists that hold them are as long as a frame asks (FShadowMaps,
// FRtPass), and every caster whose shadow can be seen is in them. What a preset says is how many are taken whatever
// their shadows reach (FShadowPreset::MaxCasters).
enum { ShadowMaxLights = RDX_SHADOW_MAX_LIGHTS };

// The factor shader's constant buffer, value by value - the ONE place its layout is written down, for both
// FBackend::FillShadowConstants and the shader's own cbuffer (ShadowFactor.hlsl).
//
//   0..3    TanX TanY Scale Count        12..15  AxisY ActorTexel      28..43  SkipRect[4]
//   4..7    NoOnTop Flags Calib MaxShare 16..19  AxisZ WorldTexel      44..47  SkipCount Eye
//   8..11   AxisX Near                   20..23  Taps Marks Spread LightSize        48..    the light arrays
//
// The three light arrays are LAST on purpose: anything written after them is addressed by a hand-written index, and
// raising the light count would shift every one of those silently, with no compiler to catch it. Nothing follows
// them, so the count is one number to change.
enum
{
    ShadowCbLightSize = 23,                                        // float: the RT shadows' lights' radius
    ShadowCbViewRect  = 24,                                        // float4: the world view's part of the target
    ShadowCbSkipRect  = 28,                                        // float4 x 4: parts drawn later, left unshadowed
    ShadowCbSkipCount = 44,                                        // uint
    ShadowCbEye       = 45,                                        // float3: the camera in world space, for the RT rays
    ShadowCbLightCam  = 48,                                        // float4 a light: xyz camera space, w radius
    ShadowCbLightInfo = ShadowCbLightCam  + 4 * ShadowMaxLights,   // x the cube slot, y brightness 0..1, z weight 0..1
    ShadowCbLightSpot = ShadowCbLightInfo + 4 * ShadowMaxLights,   // xyz axis, w cosine of the half angle
    ShadowCbCount     = ShadowCbLightSpot + 4 * ShadowMaxLights,   // values in all
};

// The RT shadows' lights (ShadowFactor.hlsl with RDX_RT, docs/terv-rt.md): discs this wide (radius, engine units) facing
// the surface, so a shadow is sharp where the blocker touches the surface and softer the further it stands off. The
// engine's lights have no size of their own; a bulb in its fitting is a few units across. The maps' soft edge is a fixed
// angle instead (GShadowSoftTexels), the same at contact and far off.
const float RtShadowLightRadius = 4.0f;
// The RT factor's two costs (ShadowFactor.hlsl, RtKnobs): the most lights a pixel traces rays to - its largest shares -
// and the samples a pixel adds where a shadow's edge crosses its quad. Measured with the bench (SecondRevision-rt-shadow.txt
// = 5) on four saves: the edge samples are half the factor's time, each 0.4-0.7 ms, while 2 and 4 of them show no
// difference in the factor view (the office of SHADOW8, the park of SHADOW1). Two it is: 3.1-3.6 ms against 4.3-4.9.
// The lights: 0, every light of the frame with a share - the RT qualities take the maps' lights, Low's, Medium's or High's,
// and the maps' graduated pick; a cut at the few largest per pixel would let a shadow come and go as shares cross.
enum { RtShadowTracedLights = 0, RtShadowEdgeSamples = 2 };

// The RT scene's copy of the level (FRtPass::SetLevel): its long triangles cut in two across their longest edge, again and
// again, until no edge is longer than MaxEdge. A BSP level is full of slivers thousands of units long and a few wide, and
// a triangle's box is what a ray meets first: one sliver's box spans a whole hall, and every ray through the hall has to
// look inside it. Measured on UNATCO Island (95553 triangles): a ray cost three times what it did at UNATCO HQ. Depth-
// limited, so a sky box's giant faces cannot run away with it: at most 2^LevelSplitDepth pieces a triangle.
enum { LevelSplitDepth = 10 };

inline float SplitDist2(const float* P, const float* Q)
{
    const float X = P[0] - Q[0], Y = P[1] - Q[1], Z = P[2] - Q[2];
    return X * X + Y * Y + Z * Z;
}

// The midpoint of the longest edge, and the two halves it makes, in the triangle's own winding: A B C cut on AB is A M C
// and M B C; on BC, A B M and A M C; on CA, A B M and M B C. Returns 0 when the triangle is left whole.
inline int SplitLongestEdge(const float* A, const float* B, const float* C, float MaxEdge2, int Depth, float* M,
                            const float** First, const float** Second)
{
    const float AB = SplitDist2(A, B), BC = SplitDist2(B, C), CA = SplitDist2(C, A);
    const float Longest = (AB > BC) ? ((AB > CA) ? AB : CA) : ((BC > CA) ? BC : CA);
    if (Longest <= MaxEdge2 || Depth <= 0)
        return 0;
    const float* P;
    const float* Q;
    if (AB >= BC && AB >= CA)      { P = A; Q = B; }
    else if (BC >= CA)             { P = B; Q = C; }
    else                           { P = C; Q = A; }
    for (int k = 0; k < 3; k++)
        M[k] = 0.5f * (P[k] + Q[k]);
    if (P == A)      { First[0] = A; First[1] = M; First[2] = C;  Second[0] = M; Second[1] = B; Second[2] = C; }
    else if (P == B) { First[0] = A; First[1] = B; First[2] = M;  Second[0] = A; Second[1] = M; Second[2] = C; }
    else             { First[0] = A; First[1] = B; First[2] = M;  Second[0] = M; Second[1] = B; Second[2] = C; }
    return 1;
}

// How many pieces SplitTriangle makes of one triangle.
inline int SplitTriangleCount(const float* A, const float* B, const float* C, float MaxEdge2, int Depth)
{
    float M[3];
    const float* First[3];
    const float* Second[3];
    if (!SplitLongestEdge(A, B, C, MaxEdge2, Depth, M, First, Second))
        return 1;
    return SplitTriangleCount(First[0], First[1], First[2], MaxEdge2, Depth - 1) +
           SplitTriangleCount(Second[0], Second[1], Second[2], MaxEdge2, Depth - 1);
}

// The pieces, nine floats each, written from Out on; returns where the next goes.
inline float* SplitTriangle(const float* A, const float* B, const float* C, float MaxEdge2, int Depth, float* Out)
{
    float M[3];
    const float* First[3];
    const float* Second[3];
    if (!SplitLongestEdge(A, B, C, MaxEdge2, Depth, M, First, Second))
    {
        for (int k = 0; k < 3; k++)
        {
            Out[k]     = A[k];
            Out[3 + k] = B[k];
            Out[6 + k] = C[k];
        }
        return Out + 9;
    }
    Out = SplitTriangle(First[0], First[1], First[2], MaxEdge2, Depth - 1, Out);
    return SplitTriangle(Second[0], Second[1], Second[2], MaxEdge2, Depth - 1, Out);
}

// The longest edge the split may leave: the shortest of these that keeps the whole level within Budget triangles - the
// finer the better for the rays, and the cost is the structure's memory and the one build at the level's load. 0 when
// even the coarsest would pass the budget: the level goes in as it is.
inline float LevelSplitEdge(const float* Xyz, int Triangles, int Budget)
{
    static const float Edges[6] = { 128.0f, 256.0f, 512.0f, 1024.0f, 2048.0f, 4096.0f };
    for (int e = 0; e < 6; e++)
    {
        const float Max2 = Edges[e] * Edges[e];
        long long Total = 0;
        for (int t = 0; t < Triangles && Total <= Budget; t++)
            Total += SplitTriangleCount(Xyz + t * 9, Xyz + t * 9 + 3, Xyz + t * 9 + 6, Max2, LevelSplitDepth);
        if (Total <= Budget)
            return Edges[e];
    }
    return 0.0f;
}

// Engine units in a metre: the usual Unreal convention (16 units a foot). Not measured in Deus Ex.
const float UnitsPerMetre = 52.5f;

struct FShadowPreset
{
    int   MaxLights;        // lights casting shadows at once
    float Reach;            // engine units: a light counts when its sphere comes this near the eye; casters within it
    int   ActorMapSize;     // a cube face's edge in texels, the actors' maps (characters, decorations, the player)
    int   WorldMapSize;     // ... the level geometry's maps - they only tell whether a wall stood between: coarser will do
    // The casters a frame takes whatever their shadows reach: with more within reach, every one whose shadow can be
    // seen is taken, and the others only while the frame's are fewer than this (RenderDevice.cpp, ChooseShadowCasters).
    int   MaxCasters;
    int   FilterTaps;       // samples a lookup takes for the soft edge
    bool  bFullResolution;  // the shadow factor at the output's resolution (half otherwise)
    // Of MaxLights, the nearest this many at full strength; the rest fade out with their distance (ShadowLightWeight).
    // LAST in the struct on purpose: it came after the brace initialisers were written, and a field put in the middle
    // would shift every one of them silently - an int landing where a float was, with no more than a warning.
    int   FullLights;
};

// The presets. More lights is NOT a better picture: every light takes a share of the pixel, the factor shader scales
// the shares down together once they sum above one, and past about ten lights no single shadow stays strong enough to
// see while the compensation lifts what is left. What separates the levels is a GRADUATED pick instead - the nearest
// FullLights at full strength, the rest fading out with distance (ShadowLightWeight) - so a light entering or leaving
// the pick does so at a weight of zero. Without that fade a light swapping in or out makes a picture on the wall jump
// in brightness from one frame to the next.
// Low 5+5, Medium 7+7, High 10+10. Every other field is the same in all three.
//
// The count is what the picture costs, too: the factor shader loops over the lights for every pixel. In one series
// on the corridor scene, 10 lights ran at 177 FPS, 25 at 140 and 50 at 61.
inline FShadowPreset ShadowPresetFor(EShadowQuality Q)
{
    FShadowPreset P = { 0, 0.0f, 0, 0, 0, 0, false, 0 };
    switch (ShadowMapQualityFor(Q))
    {
    case ShadowQuality_High:
        P.MaxLights = 20; P.FullLights = 10; P.Reach = 100.0f * UnitsPerMetre; P.ActorMapSize = 1024;
        P.WorldMapSize = 512; P.MaxCasters = 128; P.FilterTaps = 16; P.bFullResolution = true;
        break;
    case ShadowQuality_Medium:
        P.MaxLights = 14; P.FullLights = 7; P.Reach = 75.0f * UnitsPerMetre; P.ActorMapSize = 1024;
        P.WorldMapSize = 512; P.MaxCasters = 128; P.FilterTaps = 16; P.bFullResolution = true;
        break;
    case ShadowQuality_Low:
        P.MaxLights = 10; P.FullLights = 5; P.Reach = 50.0f * UnitsPerMetre; P.ActorMapSize = 1024;
        P.WorldMapSize = 512; P.MaxCasters = 128; P.FilterTaps = 16; P.bFullResolution = true;
        break;
    default:
        break;
    }
    return P;
}

// Whether two presets need the SAME shadow maps: the three fields the maps are built from (FShadowMaps::Init - the
// lights, and the two cube sizes). Everything else in a preset - the reach, the casters, the taps, the factor's
// resolution, the lights at full strength - is read frame by frame, so a change of only that keeps the maps, the level's
// geometry in them and every shape placed in their arena.
inline bool ShadowMapsSame(const FShadowPreset& A, const FShadowPreset& B)
{
    return A.MaxLights == B.MaxLights && A.ActorMapSize == B.ActorMapSize && A.WorldMapSize == B.WorldMapSize;
}

// The ini's text for ShadowQuality: Off, Low, Medium, High, RT Low, RT Medium or RT High in any case (or the number,
// 0-6), spaces around it allowed; within it a space, '-' or '_' counts for nothing, so RT-Low, rt low and RTLOW are one,
// and RT alone is RT High. Anything else gives High - the default - with bKnown false, so the caller can say so in the
// log. A template: the engine's TCHAR, the tests' char and wchar_t alike.
template <typename TChar>
inline EShadowQuality ParseShadowQuality(const TChar* Text, bool& bKnown)
{
    static const char* const Names[8] = { "off", "low", "medium", "high", "rt", "rtlow", "rtmedium", "rthigh" };
    static const EShadowQuality Values[8] = { ShadowQuality_Off, ShadowQuality_Low, ShadowQuality_Medium,
                                              ShadowQuality_High, ShadowQuality_Rt, ShadowQuality_RtLow,
                                              ShadowQuality_RtMedium, ShadowQuality_Rt };
    bKnown = false;
    if (!Text)
        return ShadowQuality_High;
    // The text in lower case, without the spaces, '-' and '_' - at most 15 letters; a longer one is nothing known.
    char Word[16];
    int  Len = 0;
    for (int i = 0; Text[i]; i++)
    {
        unsigned C = (unsigned)Text[i];
        if (C == ' ' || C == '\t' || C == '-' || C == '_')
            continue;
        if (Len == 15)
            return ShadowQuality_High;
        if (C >= 'A' && C <= 'Z')
            C += 'a' - 'A';
        Word[Len++] = (char)((C < 128u) ? C : '?');
    }
    Word[Len] = 0;
    if (Len == 1 && Word[0] >= '0' && Word[0] < (char)('0' + ShadowQuality_Count))
    {
        bKnown = true;
        return (EShadowQuality)(Word[0] - '0');
    }
    for (int q = 0; q < 8; q++)
    {
        int k = 0;
        while (Names[q][k] && Names[q][k] == Word[k])
            k++;
        if (Names[q][k] == 0 && Word[k] == 0)
        {
            bKnown = true;
            return Values[q];
        }
    }
    return ShadowQuality_High;
}

// A cube map's six faces in Direct3D's order: +X, -X, +Y, -Y, +Z, -Z. For a direction D the sampler takes the face of D's
// largest component (ma), and on it s = (sc / |ma| + 1) / 2, t = (tc / |ma| + 1) / 2, t growing downwards - the table of
// the Direct3D and OpenGL specifications, which CubeFaceLookup writes out. A face's own axes follow from that table: Right
// grows s, Down grows t, Forward is the face's direction. World coordinates go in as they are (UE1: X forward, Y right,
// Z up): the sampler does not care what the axes mean, only that the maps are drawn and read by the same table.
inline void CubeFaceAxes(int Face, FVec3& Right, FVec3& Down, FVec3& Forward)
{
    static const float Table[6][9] =
    {   //  Right          Down            Forward
        {  0,  0, -1,     0, -1,  0,      1,  0,  0 },   // +X: sc = -z, tc = -y
        {  0,  0,  1,     0, -1,  0,     -1,  0,  0 },   // -X: sc = +z, tc = -y
        {  1,  0,  0,     0,  0,  1,      0,  1,  0 },   // +Y: sc = +x, tc = +z
        {  1,  0,  0,     0,  0, -1,      0, -1,  0 },   // -Y: sc = +x, tc = -z
        {  1,  0,  0,     0, -1,  0,      0,  0,  1 },   // +Z: sc = +x, tc = -y
        { -1,  0,  0,     0, -1,  0,      0,  0, -1 },   // -Z: sc = -x, tc = -y
    };
    const float* T = Table[(Face >= 0 && Face < 6) ? Face : 0];
    Right.X   = T[0]; Right.Y   = T[1]; Right.Z   = T[2];
    Down.X    = T[3]; Down.Y    = T[4]; Down.Z    = T[5];
    Forward.X = T[6]; Forward.Y = T[7]; Forward.Z = T[8];
}

// The specifications' table itself, case by case: the face a direction samples, and s and t on it. The tests hold
// CubeFaceAxes and CubeFaceMatrix against this.
inline int CubeFaceLookup(const FVec3& D, float& S, float& T)
{
    const float AX = (D.X < 0.0f) ? -D.X : D.X;
    const float AY = (D.Y < 0.0f) ? -D.Y : D.Y;
    const float AZ = (D.Z < 0.0f) ? -D.Z : D.Z;
    int   Face;
    float Sc, Tc, Ma;
    if (AX >= AY && AX >= AZ)
    {
        Ma = AX;
        if (D.X >= 0.0f) { Face = 0; Sc = -D.Z; Tc = -D.Y; }
        else             { Face = 1; Sc =  D.Z; Tc = -D.Y; }
    }
    else if (AY >= AZ)
    {
        Ma = AY;
        if (D.Y >= 0.0f) { Face = 2; Sc =  D.X; Tc =  D.Z; }
        else             { Face = 3; Sc =  D.X; Tc = -D.Z; }
    }
    else
    {
        Ma = AZ;
        if (D.Z >= 0.0f) { Face = 4; Sc =  D.X; Tc = -D.Y; }
        else             { Face = 5; Sc = -D.X; Tc = -D.Y; }
    }
    S = (Ma > 0.0f) ? 0.5f * (Sc / Ma + 1.0f) : 0.5f;
    T = (Ma > 0.0f) ? 0.5f * (Tc / Ma + 1.0f) : 0.5f;
    return Face;
}

// The matrix a face is drawn with, row by row, for a column (P, 1) in world units, L the light: x = Right . (P - L),
// y = -Down . (P - L) (clip space's y grows upwards, t downwards), w = Forward . (P - L), z = Near. So x / w and y / w are
// the face's s and t over -1..1 (a 90-degree view), and the depth Near / w is reversed with no far plane: 1 at Near units
// from the light, towards 0 with distance - the scene's depth buffer works the same way (DepthFromViewZ). Anything nearer
// than Near to the light is cut off (z / w > 1).
inline void CubeFaceMatrix(int Face, const FVec3& Light, float Near, float M[16])
{
    FVec3 R, D, F;
    CubeFaceAxes(Face, R, D, F);
    M[0]  =  R.X; M[1]  =  R.Y; M[2]  =  R.Z; M[3]  = -Dot(R, Light);
    M[4]  = -D.X; M[5]  = -D.Y; M[6]  = -D.Z; M[7]  =  Dot(D, Light);
    M[8]  = 0.0f; M[9]  = 0.0f; M[10] = 0.0f; M[11] =  Near;
    M[12] =  F.X; M[13] =  F.Y; M[14] =  F.Z; M[15] = -Dot(F, Light);
}

// What a face's depth holds for a surface at V = P - L: Near over V's largest component (see CubeFaceMatrix) - the
// lookup's reference. Within Near: 1.
inline float CubeDepth(const FVec3& V, float Near)
{
    const float AX = (V.X < 0.0f) ? -V.X : V.X;
    const float AY = (V.Y < 0.0f) ? -V.Y : V.Y;
    const float AZ = (V.Z < 0.0f) ? -V.Z : V.Z;
    const float Ma = (AX > AY) ? ((AX > AZ) ? AX : AZ) : ((AY > AZ) ? AY : AZ);
    return (Ma > Near) ? Near / Ma : 1.0f;
}

// Whether a sphere (its centre V relative to the light, radius Radius) reaches into a face's view: the view is bounded by
// the four planes through the light where |x| = w and |y| = w (see CubeFaceMatrix), each at 45 degrees - so a centre at
// most Radius * sqrt(2) outside each of them. Conservative: a sphere near a corner may pass without touching.
inline bool SphereTouchesCubeFace(int Face, const FVec3& V, float Radius)
{
    FVec3 R, D, F;
    CubeFaceAxes(Face, R, D, F);
    const float X = Dot(R, V), Y = Dot(D, V), W = Dot(F, V);
    const float Slack = Radius * 1.41421356f;
    return W - X >= -Slack && W + X >= -Slack && W - Y >= -Slack && W + Y >= -Slack;
}

// The faces a sphere reaches into, bit f for face f: SphereTouchesCubeFace's comparisons for all six faces at once, each
// face's axes written out. CubeFaceAxes has signed unit vectors only, so each dot product there is a component with its sign
// and the sums here are the same floating-point values - the tests hold the two against each other. FShadowMaps::Render
// sorts a world map's triangles to their faces with it in one pass instead of six (the turning's CPU spikes,).
inline unsigned CubeFacesTouched(const FVec3& V, float Radius)
{
    const float Lim = -(Radius * 1.41421356f);
    const float X = V.X, Y = V.Y, Z = V.Z;
    unsigned Mask = 0;
    if (X + Z >= Lim && X - Z >= Lim && X + Y >= Lim && X - Y >= Lim)         // +X: W = x, X = -z, Y = -y
        Mask |= 1u;
    if (-X - Z >= Lim && -X + Z >= Lim && -X + Y >= Lim && -X - Y >= Lim)     // -X: W = -x, X = z, Y = -y
        Mask |= 2u;
    if (Y - X >= Lim && Y + X >= Lim && Y - Z >= Lim && Y + Z >= Lim)         // +Y: W = y, X = x, Y = z
        Mask |= 4u;
    if (-Y - X >= Lim && -Y + X >= Lim && -Y + Z >= Lim && -Y - Z >= Lim)     // -Y: W = -y, X = x, Y = -z
        Mask |= 8u;
    if (Z - X >= Lim && Z + X >= Lim && Z + Y >= Lim && Z - Y >= Lim)         // +Z: W = z, X = x, Y = -y
        Mask |= 16u;
    if (-Z + X >= Lim && -Z - X >= Lim && -Z + Y >= Lim && -Z - Y >= Lim)     // -Z: W = -z, X = -x, Y = -y
        Mask |= 32u;
    return Mask;
}

// ---------------------------------------------------------------------------
// The shadows' compensation. The shadows take a share of the picture's light and
// this gives it back in two stages, in this order: the lamps are turned up FIRST, by at most half - above that the
// picture goes unreadable in places - and only if that is not enough are the shadows themselves eased, never below
// HALF their own strength, so that shadows remain visible. Whatever darkening is left after both stages is accepted.
// Neither stage is a setting: the menu turns the shadows on and off, and that is the only hand on any of it.
//
// The lamps are multiplied in the geometry pass, so an unlit place stays dark (zero times anything is zero) - a dark
// tunnel is not lifted, only what a lamp actually reaches.
// ---------------------------------------------------------------------------

// The lamps may add at most this much.
static const float ShadowBoostMax = 0.50f;
// The shadows are never eased below this share of their own strength.
static const float ShadowStrengthFloor = 0.50f;
// There is no third number: the compensation gives back ALL of the darkening, and the two ceilings above are its only
// brakes. A "residual darkening we can live with" would be a third one, and it would stop the system before either
// ceiling was ever reached - what a person can tolerate is not the same thing as where the controller should stop.

// What the compensation asks for: the lamps' multiplier (1.0 = the engine's own light) and the shadows' strength
// (1.0 = the maps' own darkening).
struct FShadowCompensation
{
    float Boost;
    float Strength;
};

// The two stages, worked out from the shadow metric - the average of the shadow factor, where 1.0 means the shadows took
// nothing and 0.75 that a quarter of the light is gone - and from the strength that was IN FORCE WHEN IT WAS MEASURED.
//
// That second argument is what keeps the loop from running away. Stage two changes the strength, which changes the next
// measurement; dividing the measured loss by the strength it was measured at recovers the loss AT FULL STRENGTH, and
// that is the one number stage two does not move. Without it the controller would be reading its own output: easing the
// shadows would improve the metric, which would ease the compensation, which would darken the picture again.
//
// BoostMax and StrengthFloor are the two limits, and they are arguments rather than constants because HDR moves
// both: with range above white to spend, the lamps may go further (RdxMath::ShadowBoostCeiling) and stage two is
// not needed at all (ShadowStrengthFloorFor returns 1, so the shadows keep full strength). The defaults are the
// SDR values, so every caller that does not care reads exactly as it did.
inline FShadowCompensation CompensateShadows(float Metric, float MetricStrength,
                                             float BoostMax      = ShadowBoostMax,
                                             float StrengthFloor = ShadowStrengthFloor)
{
    FShadowCompensation R;
    R.Boost    = 1.0f;
    R.Strength = 1.0f;
    // No sample yet (-1), the shadows took nothing (1.0), or it was measured with them off: leave everything alone.
    if (!(Metric >= 0.0f) || Metric >= 1.0f || !(MetricStrength > 0.0f))
        return R;
    float Full = (1.0f - Metric) / MetricStrength;   // what they would take at full strength
    if (Full > 1.0f)
        Full = 1.0f;
    const float Keep = 1.0f - Full;                  // the light left at full strength
    // Stage one: give ALL of it back with the lamps, as far as their ceiling allows.
    const float Most = 1.0f + ((BoostMax > 0.0f) ? BoostMax : 0.0f);
    R.Boost = (Keep > 0.0f) ? (1.0f / Keep) : Most;
    if (R.Boost > Most)
        R.Boost = Most;
    if (Keep * R.Boost >= 1.0f - 1.0e-4f)            // the slack is float noise, not a tolerance
        return R;                                    // the lamps alone reached it
    // Stage two: ease the shadows for the rest - (1 - Full * Strength) * Boost = 1 - down to the floor and no further.
    // We only get here when the boost hit its ceiling, which needs Keep < 1/Most, that is Full > 1/3: never a division
    // by zero, and never for a scene the lamps alone could carry.
    float S = (1.0f - 1.0f / R.Boost) / Full;
    if (S > 1.0f)
        S = 1.0f;
    if (S < StrengthFloor)
        S = StrengthFloor;
    R.Strength = S;
    return R;
}

// What is LEFT of the darkening once the lamps have given back what they can: the measured light ratio times the boost
// actually applied, as a darkening again. Zero is what the two stages aim at, and this is the honest measure of
// whether the compensation is doing its job. It is model-based rather than measured: the boost deliberately never
// touches the light target the metric reads (see FSolidPass::SetLightBoost), so no measurement of ours can see the
// lamps' own effect.
//
// Multiplying is right rather than optimistic: the shadows take a SHARE of whatever light a pixel receives, so turning
// the lamps up scales both the light and what is taken. Where no lamp reaches, both sides are zero and it says nothing.
inline float CompensatedDarkening(float Metric, float Boost)
{
    if (!(Metric >= 0.0f))
        return 0.0f;
    const float Left = 1.0f - Metric * ((Boost > 0.0f) ? Boost : 1.0f);
    return (Left > 0.0f) ? Left : 0.0f;
}

// One step of the slow follow, used by both stages. The metric jumps as the view turns, and a compensation that jumped
// with it would make the picture breathe, so the applied value walks towards the target rather than snapping to it.
// Rate is the share of the gap crossed in this step; a small value and a per-frame call give a settle of a second or two.
inline float FollowSlowly(float Now, float Target, float Rate)
{
    if (!(Now >= 0.0f))          // a NaN would stay one for ever
        return Target;
    const float R = (Rate < 0.0f) ? 0.0f : ((Rate > 1.0f) ? 1.0f : Rate);
    return Now + (Target - Now) * R;
}

// The same follow over TIME instead of frames (the graduated shadow pick): a light's weight has to settle in the same
// quarter of a second at 60 FPS and at 150, and a per-frame rate would take two and a half times as long at the lower
// one. Dt / (Tau + Dt) stands for 1 - exp(-Dt / Tau) without a library (this header takes none): within a few percent of
// it at a frame's Dt, and never above 1 however long the hitch - a second's stall at Tau 0.25 crosses 80% of the gap.
inline float FollowOverTime(float Now, float Target, float Dt, float Tau)
{
    if (!(Dt > 0.0f))
        return (Now >= 0.0f) ? Now : Target;
    if (!(Tau > 0.0f))
        return Target;
    return FollowSlowly(Now, Target, Dt / (Tau + Dt));
}

// ---------------------------------------------------------------------------
// Upscaling (docs/terv-latvany-roadmap.md 2.3;  Off, Performance, Balanced, Quality). The scene is
// drawn at the output's size over the mode's ratio in each direction and scaled up to the output before the HUD, which
// keeps the output's size. The ratios are FSR's.
// ---------------------------------------------------------------------------

enum EUpscaling
{
    Upscaling_Off         = 0,
    Upscaling_NativeAA    = 1,   // 1.0 - no upscaling at all, only the temporal antialiasing
    Upscaling_Quality     = 2,   // 1.5
    Upscaling_Balanced    = 3,   // 1.7
    Upscaling_Performance = 4    // 2
};

// The textures' mip bias under the temporal upscaler (docs/terv-latvany-roadmap.md 2.6). The GPU picks a texture's mip
// level for the scene's own resolution, which under FSR is below the output's - so the textures come out as blurred as
// the smaller picture, and the upscaler cannot bring back detail that was never sampled. AMD's rule for FSR 3.1 is
// log2(render / output) - 1: Quality -1.58, Balanced -1.77, Performance -2. NativeAA upscales nothing, and AMD's table
// gives it no value; the rule would say -1, which is to be decided on the picture - so 0 unless asked (bNative).
// Anything else, 0: without the temporal accumulation a negative bias only shimmers.
inline float FsrMipBias(EUpscaling Mode, bool bNative)
{
    switch (Mode)
    {
    case Upscaling_Quality:     return -1.5849625f;     // log2(1 / 1.5) - 1
    case Upscaling_Balanced:    return -1.7655347f;     // log2(1 / 1.7) - 1
    case Upscaling_Performance: return -2.0f;           // log2(1 / 2) - 1
    case Upscaling_NativeAA:    return bNative ? -1.0f : 0.0f;
    default:                    return 0.0f;
    }
}

// XeSS's rule for the same is log2(render / output) - its guide's values are that alone, without FSR's -1 - and
// NativeAA, which upscales nothing, gets none.
inline float XessMipBias(EUpscaling Mode)
{
    switch (Mode)
    {
    case Upscaling_Quality:     return -0.5849625f;     // log2(1 / 1.5)
    case Upscaling_Balanced:    return -0.7655347f;     // log2(1 / 1.7)
    case Upscaling_Performance: return -1.0f;           // log2(1 / 2)
    default:                    return 0.0f;
    }
}

inline float UpscalingRatio(EUpscaling Mode)
{
    switch (Mode)
    {
    case Upscaling_Quality:     return 1.5f;
    case Upscaling_Balanced:    return 1.7f;
    case Upscaling_Performance: return 2.0f;
    default:                    return 1.0f;
    }
}

// The scene's side for one of the output's: the nearest whole pixel, at least one.
inline int UpscaledSceneSize(int Output, EUpscaling Mode)
{
    const int N = (int)((float)Output / UpscalingRatio(Mode) + 0.5f);
    return (N < 1) ? 1 : N;
}

// The two settings COMPOSE, and neither cancels the other. They pull in opposite directions, and that is the user's business, not ours:
//
// output  ---x SuperSample-->  the antialiased image  ---/ ratio -->  the scene actually drawn
//
// Supersampling raises the size of the image the upscaler produces; the upscaling mode then says how much SMALLER
// than that the scene is drawn. What comes back down to the output is a box average of the antialiased image.
// Performance with SuperSample 2 therefore draws at the output's own size, upscales to twice it, and averages it
// back down - honest work for nothing, which is why the setting is documented, not silently overridden.
// The largest supersampling factor whose scene target still fits what D3D12 allows for a 2D texture
// (D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION = 16384 on every feature level we run on). At 3840x2160 that is 4
// (15360x8640); at 1920x1080 it is 8. Anything above is refused rather than silently drawn smaller.
enum { MaxTextureSide = 16384 };

// And on top of that the setting's own ceiling: 4. 4x is 16 times the pixel work - the knob
// for making the GPU, and not the engine, the thing that limits the frame rate.
enum { SuperSampleMax = 4 };

inline int MaxSceneScale(int OutputWidth, int OutputHeight)
{
    const int W = (OutputWidth  > 0) ? OutputWidth: 1;
    const int H = (OutputHeight > 0) ? OutputHeight : 1;
    const int S = (MaxTextureSide / ((W > H) ? W : H));
    return (S < 1) ? 1 : ((S > (int)SuperSampleMax) ? (int)SuperSampleMax : S);
}

inline int AntialiasedSideFor(int Output, int SuperSample)
{
    const int S = (SuperSample < 1) ? 1 : SuperSample;
    return (Output < 1) ? 1 : Output * S;
}

inline int SceneSideFor(int Output, int SuperSample, EUpscaling Mode)
{
    return UpscaledSceneSize(AntialiasedSideFor(Output, SuperSample), Mode);
}

// What the passes' own targets are made by: the scene's size, the picture's, the colours' format and the shadow
// factor's divisor.
struct FTargetAsk
{
    int SceneW, SceneH, OutW, OutH, Colours, FactorDiv;

    FTargetAsk() : SceneW(0), SceneH(0), OutW(0), OutH(0), Colours(0), FactorDiv(0) {}
    bool Same(const FTargetAsk& O) const
    {
        return SceneW == O.SceneW && SceneH == O.SceneH && OutW == O.OutW && OutH == O.OutH && Colours == O.Colours &&
               FactorDiv == O.FactorDiv;
    }
};

// A pass's own target that could not be made, and what was asked then. A failing allocation asked again every frame
// would stall every frame, so the pass is left out while the ask stands - and asked again once anything in it is
// another. Anything, not the pass's own size alone: a target of the picture's size has more of the card's memory left
// to it under a smaller scene.
struct FTargetRefusal
{
    FTargetAsk Ask;
    bool       bRefused;

    FTargetRefusal() : bRefused(false) {}
    bool Stands(const FTargetAsk& Now) const { return bRefused && Ask.Same(Now); }
    void Note(const FTargetAsk& Now)
    {
        Ask      = Now;
        bRefused = true;
    }
    // Once a frame, with what is asked now: a refusal lasts while that stands and is gone for good once it is another.
    // Otherwise the way back to the sizes it was made at - the ones the player plays at, as likely as not - would
    // find it standing again, with the card's memory long since another.
    void Keep(const FTargetAsk& Now)
    {
        if (bRefused && !Ask.Same(Now))
            bRefused = false;
    }
};

// Which of Names the text is - in any case, with spaces, tabs or a carriage return around it - or -1 for none of them.
template <typename TChar>
inline int NameIndex(const TChar* Text, const char* const* Names, int Count)
{
    if (!Text)
        return -1;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    int E = B;
    while (Text[E])
        E++;
    while (E > B && (Text[E - 1] == ' ' || Text[E - 1] == '\t' || Text[E - 1] == '\r'))
        E--;
    for (int q = 0; q < Count; q++)
    {
        const char* N = Names[q];
        int k = 0;
        for (; k < E - B && N[k]; k++)
        {
            unsigned C = (unsigned)Text[B + k];
            if (C >= 'A' && C <= 'Z')
                C += 'a' - 'A';
            unsigned D = (unsigned)N[k];
            if (D >= 'A' && D <= 'Z')
                D += 'a' - 'A';
            if (C != D)
                break;
        }
        if (k == E - B && N[k] == 0)
            return q;
    }
    return -1;
}

// The Upscale key (and the menu's UPSCALE line): how much smaller than the output the scene is drawn under a temporal
// upscaler - FSR3, FSR4 or DLSS of the AntiAliasing key. Off is not one of its values: whether anything upscales at all
// is the antialiasing's to say.
inline const char* const* UpscaleNames()
{
    static const char* const Names[4] = { "NativeAA", "Quality", "Balanced", "Performance" };
    return Names;
}

inline const char* UpscaleName(EUpscaling U)
{
    return UpscaleNames()[(U >= Upscaling_NativeAA && U <= Upscaling_Performance) ? (int)U - 1 : 0];
}

// The key's text; NativeAA - the default - for anything else, with bKnown false for the log.
template <typename TChar>
inline EUpscaling ParseUpscale(const TChar* Text, bool& bKnown)
{
    const int q = NameIndex(Text, UpscaleNames(), 4);
    bKnown = q >= 0;
    return bKnown ? (EUpscaling)(q + 1) : Upscaling_NativeAA;
}

// The menu's next size, either way round: NativeAA, Quality, Balanced, Performance.
inline EUpscaling UpscaleStep(EUpscaling U, int Step)
{
    const int i = (U >= Upscaling_NativeAA && U <= Upscaling_Performance) ? (int)U - 1 : 0;
    return (EUpscaling)(((i + Step) % 4 + 4) % 4 + 1);
}

// ---------------------------------------------------------------------------
// Our own settings file. The engine's ini is not ours to keep.
// ---------------------------------------------------------------------------
//
// Why we do not use the engine's config system for our own keys: UE1 rewrites the ini it owns when it shuts down -
// measured, not guessed.
// A settings file that cannot carry its own documentation is a settings file nobody can use without a manual, and a
// file the game rewrites behind the player's back is not the player's.
//
// So our keys live in a file the engine has never heard of, and this is its reader. The format is the plain one
// everyone knows: "Key=Value" a line, ';' or '#' starts a comment, a "[section]" line is skipped. Whitespace around
// the key and the value does not count. The first match wins.
inline bool SettingsFind(const char* Text, const char* Key, char* Out, int Max)
{
    if (Out && Max > 0)
        Out[0] = 0;
    if (!Text || !Key || !Out || Max <= 0)
        return false;
    int KeyLen = 0;
    while (Key[KeyLen])
        KeyLen++;
    int i = 0;
    while (Text[i])
    {
        // The start of a line, past any spaces.
        int b = i;
        while (Text[b] == ' ' || Text[b] == '\t')
            b++;
        // Where this line ends.
        int e = b;
        while (Text[e] && Text[e] != '\n' && Text[e] != '\r')
            e++;
        const int Next = Text[e] ? (e + 1) : e;
        if (Text[b] != ';' && Text[b] != '#' && Text[b] != '[' && Text[b] != 0 && b < e)
        {
            // Does the key match, and is the next thing an '=' (spaces allowed before it)?
            int k = 0;
            while (k < KeyLen && b + k < e)
            {
                unsigned C = (unsigned)Text[b + k];
                unsigned D = (unsigned)Key[k];
                if (C >= 'A' && C <= 'Z') C += 'a' - 'A';
                if (D >= 'A' && D <= 'Z') D += 'a' - 'A';
                if (C != D)
                    break;
                k++;
            }
            if (k == KeyLen)
            {
                int v = b + KeyLen;
                while (v < e && (Text[v] == ' ' || Text[v] == '\t'))
                    v++;
                if (v < e && Text[v] == '=')
                {
                    v++;
                    while (v < e && (Text[v] == ' ' || Text[v] == '\t'))
                        v++;
                    int Last = e;
                    while (Last > v && (Text[Last - 1] == ' ' || Text[Last - 1] == '\t'))
                        Last--;
                    int n = 0;
                    for (; v + n < Last && n < Max - 1; n++)
                        Out[n] = Text[v + n];
                    Out[n] = 0;
                    return true;
                }
            }
        }
        if (Next == e && !Text[e])
            break;
        i = Next;
    }
    return false;
}

// The settings file's text with one key set - the renderer's menu writes its changes so. The line SettingsFind reads
// the key from gets Value in place of its own; everything else of the file stays as it was - every other line, the
// comments, that line's spaces and its end. Where no line names the key, "Key=Value" is added at the end, with the
// file's own line end. Into Out, its zero included, Max bytes at most: the length, or -1 where it does not fit.
inline int SettingsSet(const char* Text, const char* Key, const char* Value, char* Out, int Max)
{
    if (!Text || !Key || !Value || !Out || Max <= 0)
        return -1;
    int KeyLen = 0, ValueLen = 0, TextLen = 0;
    while (Key[KeyLen])
        KeyLen++;
    while (Value[ValueLen])
        ValueLen++;
    while (Text[TextLen])
        TextLen++;
    // Where the value on the key's line starts and ends - the same reading as SettingsFind's.
    int From = -1, To = -1;
    for (int i = 0; Text[i] && From < 0;)
    {
        int b = i;
        while (Text[b] == ' ' || Text[b] == '\t')
            b++;
        int e = b;
        while (Text[e] && Text[e] != '\n' && Text[e] != '\r')
            e++;
        const int Next = Text[e] ? (e + 1) : e;
        if (Text[b] != ';' && Text[b] != '#' && Text[b] != '[' && Text[b] != 0 && b < e)
        {
            int k = 0;
            while (k < KeyLen && b + k < e)
            {
                unsigned C = (unsigned)Text[b + k];
                unsigned D = (unsigned)Key[k];
                if (C >= 'A' && C <= 'Z') C += 'a' - 'A';
                if (D >= 'A' && D <= 'Z') D += 'a' - 'A';
                if (C != D)
                    break;
                k++;
            }
            if (k == KeyLen)
            {
                int v = b + KeyLen;
                while (v < e && (Text[v] == ' ' || Text[v] == '\t'))
                    v++;
                if (v < e && Text[v] == '=')
                {
                    v++;
                    while (v < e && (Text[v] == ' ' || Text[v] == '\t'))
                        v++;
                    int Last = e;
                    while (Last > v && (Text[Last - 1] == ' ' || Text[Last - 1] == '\t'))
                        Last--;
                    From = v;
                    To   = Last;
                }
            }
        }
        if (Next == e && !Text[e])
            break;
        i = Next;
    }
    int n = 0;
    if (From >= 0)
    {
        if (From + ValueLen + (TextLen - To) > Max - 1)
            return -1;
        for (int c = 0; c < From; c++)
            Out[n++] = Text[c];
        for (int c = 0; c < ValueLen; c++)
            Out[n++] = Value[c];
        for (int c = To; c < TextLen; c++)
            Out[n++] = Text[c];
        Out[n] = 0;
        return n;
    }
    // A line of its own at the end, after a line end where the file has none there yet.
    bool bCrLf = false;
    for (int c = 0; c + 1 < TextLen && !bCrLf; c++)
        bCrLf = (Text[c] == '\r' && Text[c + 1] == '\n');
    const int  EndLen   = bCrLf ? 2 : 1;
    const bool bBreak   = TextLen > 0 && Text[TextLen - 1] != '\n';
    if (TextLen + (bBreak ? EndLen : 0) + KeyLen + 1 + ValueLen + EndLen > Max - 1)
        return -1;
    for (int c = 0; c < TextLen; c++)
        Out[n++] = Text[c];
    if (bBreak)
    {
        if (bCrLf)
            Out[n++] = '\r';
        Out[n++] = '\n';
    }
    for (int c = 0; c < KeyLen; c++)
        Out[n++] = Key[c];
    Out[n++] = '=';
    for (int c = 0; c < ValueLen; c++)
        Out[n++] = Value[c];
    if (bCrLf)
        Out[n++] = '\r';
    Out[n++] = '\n';
    Out[n] = 0;
    return n;
}

// On or Off in any case; a missing key, or one that holds neither, keeps the default.
inline bool SettingsBool(const char* Text, const char* Key, bool bDefault)
{
    char V[32];
    if (!SettingsFind(Text, Key, V, 32))
        return bDefault;
    const auto Is = [&V](const char* W) {
        int k = 0;
        for (; V[k] && W[k]; k++)
            if (((V[k] >= 'A' && V[k] <= 'Z') ? (char)(V[k] - 'A' + 'a') : V[k]) != W[k])
                return false;
        return !V[k] && !W[k];
    };
    return Is("on") ? true : (Is("off") ? false : bDefault);
}

// A decimal number; a missing or unreadable key keeps the default.
inline float SettingsFloat(const char* Text, const char* Key, float Default)
{
    char V[64];
    if (!SettingsFind(Text, Key, V, 64) || !V[0])
        return Default;
    bool  bNeg = false;
    int   i    = 0;
    if (V[0] == '-' || V[0] == '+')
    {
        bNeg = (V[0] == '-');
        i    = 1;
    }
    float Whole = 0.0f;
    bool  bAny  = false;
    for (; V[i] >= '0' && V[i] <= '9'; i++)
    {
        Whole = Whole * 10.0f + (float)(V[i] - '0');
        bAny  = true;
    }
    if (V[i] == '.' || V[i] == ',')
    {
        i++;
        float Scale = 0.1f;
        for (; V[i] >= '0' && V[i] <= '9'; i++)
        {
            Whole += (float)(V[i] - '0') * Scale;
            Scale *= 0.1f;
            bAny   = true;
        }
    }
    if (!bAny)
        return Default;
    return bNeg ? -Whole : Whole;
}

// ---------------------------------------------------------------------------
// The preset: one word that says what kind of picture this is, and it replaced the Vanilla
// switch.
//
//   Vanilla             - what the game shipped with: no shadows, no smoothing, the D3D10 renderer's picture. Every
//                         extra is off whatever its own key says.
//   Recommended         - what we would pick: shadows at High, the temporal antialiasing at full resolution and the
//                         ambient occlusion.
//   INeverAskedForThis  - Recommended with HDR on. On a display that cannot show it the output steps back by itself
//                         (HdrModeForDisplay), so it is safe to pick anywhere.
//   Custom              - the keys in the ini, as they stand.
//
// The presets IGNORE the other keys; only Custom reads them. That is the point of a preset: one line to change, and no
// wondering which of six settings is the one doing it.
// ---------------------------------------------------------------------------

enum EPreset
{
    Preset_Recommended = 0,
    Preset_INeverAskedForThis,
    Preset_Vanilla,
    Preset_Custom,
    Preset_Count
};

inline const char* const* PresetNames()
{
    static const char* const Names[Preset_Count] = { "Recommended", "INeverAskedForThis", "Vanilla", "Custom" };
    return Names;
}

// The preset as the menu says it - in words, with the spaces an ini name cannot have.
inline const char* PresetCaption(EPreset P)
{
    static const char* const Captions[Preset_Count] = { "RECOMMENDED", "I NEVER ASKED FOR THIS", "VANILLA", "CUSTOM" };
    return Captions[(P >= 0 && P < Preset_Count) ? (int)P : 0];
}

template <typename TChar>
inline void PresetName(EPreset P, TChar* Out, int Max)
{
    const char* const N = PresetNames()[(P >= 0 && P < Preset_Count) ? (int)P : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

template <typename TChar>
inline EPreset ParsePreset(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return Preset_Custom;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    int E = B;
    while (Text[E])
        E++;
    while (E > B && (Text[E - 1] == ' ' || Text[E - 1] == '\t'))
        E--;
    const int Len = E - B;
    const char* const* Names = PresetNames();
    for (int q = 0; q < (int)Preset_Count; q++)
    {
        const char* N = Names[q];
        int k = 0;
        for (; k < Len && N[k]; k++)
        {
            unsigned C = (unsigned)Text[B + k];
            if (C >= 'A' && C <= 'Z')
                C += 'a' - 'A';
            unsigned D = (unsigned)N[k];
            if (D >= 'A' && D <= 'Z')
                D += 'a' - 'A';
            if (C != D)
                break;
        }
        if (k == Len && N[k] == 0)
        {
            bKnown = true;
            return (EPreset)q;
        }
    }
    return Preset_Custom;
}

// ---------------------------------------------------------------------------
// Antialiasing as the renderer RUNS it: one mode for every method, with the temporal upscaler's size folded into the
// four FSR* values. What the player picks is EAaChoice further down - the AntiAliasing key and the menu's line - and
// AaRunFor makes one of these of it.
//
// The number in a name is SAMPLES PER PIXEL wherever it means anything: SSAA4X is the scene drawn at 2x2, MSAA4X four
// hardware samples. FXAA and SMAA work from the finished picture and take no samples, so they have no number.
// ---------------------------------------------------------------------------

enum EAntiAliasing
{
    AA_Off = 0,
    AA_Fxaa,                                     // a post-process filter; it takes no samples, so it has no number
    AA_Smaa,
    AA_Ssaa4, AA_Ssaa9, AA_Ssaa16,               // supersampling: 2x2, 3x3, 4x4 samples per pixel
    AA_Msaa2, AA_Msaa4, AA_Msaa8, AA_Msaa16,     // hardware multisampling - what the CARD gives is decided at
                                                 // run time, not here: a count it refuses steps down, with a log
    AA_FsrNativeAA, AA_FsrQuality, AA_FsrBalanced, AA_FsrPerformance,
    AA_Count
};

// The families, so a caller can ask which kind a value belongs to without listing every one.
inline bool AaIsFxaa(EAntiAliasing A) { return A == AA_Fxaa; }
inline bool AaIsSmaa(EAntiAliasing A) { return A == AA_Smaa; }
inline bool AaIsPost(EAntiAliasing A) { return A == AA_Fxaa || A == AA_Smaa; }
inline bool AaIsSsaa(EAntiAliasing A) { return A >= AA_Ssaa4 && A <= AA_Ssaa16; }
inline bool AaIsMsaa(EAntiAliasing A) { return A >= AA_Msaa2 && A <= AA_Msaa16; }
inline bool AaIsFsr (EAntiAliasing A) { return A >= AA_FsrNativeAA && A <= AA_FsrPerformance; }

// How many times the scene is drawn in each direction: 1 unless this is supersampling. SSAA4X is 2x2 - the number in
// the name is SAMPLES PER PIXEL, the same as MSAA's, because that is what the number means everywhere else.
inline int AaSceneScale(EAntiAliasing A)
{
    switch (A)
    {
    case AA_Ssaa4:  return 2;
    case AA_Ssaa9:  return 3;
    case AA_Ssaa16: return 4;
    default:        return 1;
    }
}

// How many pixels a post filter may walk along an edge. Not a setting any more: a filter that works from the
// finished picture cannot take more samples, so a number in its name would be a lie. Measured: a long
// reach smears fine, high-contrast detail (a lit building's windows at night), a short one leaves some staircase.
// Eight is the middle, and it is the shape the classic FXAA ships with.
inline int AaPostSpan(EAntiAliasing A) { return AaIsPost(A) ? 8 : 0; }

// The hardware sample count, or 0 when this is not MSAA.
inline int AaMsaaSamples(EAntiAliasing A)
{
    switch (A)
    {
    case AA_Msaa2:  return 2;
    case AA_Msaa4:  return 4;
    case AA_Msaa8:  return 8;
    case AA_Msaa16: return 16;
    default:        return 0;
    }
}

// Which upscaling mode this asks for - Off for everything that is not one of the FSR values.
inline EUpscaling AaUpscaling(EAntiAliasing A)
{
    switch (A)
    {
    case AA_FsrNativeAA:    return Upscaling_NativeAA;
    case AA_FsrQuality:     return Upscaling_Quality;
    case AA_FsrBalanced:    return Upscaling_Balanced;
    case AA_FsrPerformance: return Upscaling_Performance;
    default:                return Upscaling_Off;
    }
}

// The modes' names, for the log and the frame capture's label.
inline const char* const* AaNames()
{
    static const char* const Names[AA_Count] =
    {
        "Off", "FXAA", "SMAA",
        "SSAA4X", "SSAA9X", "SSAA16X",
        "MSAA2X", "MSAA4X", "MSAA8X", "MSAA16X",
        "FSRNativeAA", "FSRQuality", "FSRBalanced", "FSRPerformance"
    };
    return Names;
}

// The other way: the FSR mode that draws the scene at this size.
inline EAntiAliasing AaFromUpscaling(EUpscaling U)
{
    switch (U)
    {
    case Upscaling_NativeAA:    return AA_FsrNativeAA;
    case Upscaling_Quality:     return AA_FsrQuality;
    case Upscaling_Balanced:    return AA_FsrBalanced;
    case Upscaling_Performance: return AA_FsrPerformance;
    default:                    return AA_Off;
    }
}

// The mode's name, for the log.
template <typename TChar>
inline void AaName(EAntiAliasing A, TChar* Out, int Max)
{
    const char* const N = AaNames()[(A >= 0 && A < AA_Count) ? (int)A : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Whose temporal upscaler the FSR modes run with (docs/terv-fsr4.md) - the library; the mode, how much the scene
// shrinks, is EAntiAliasing's. Not a setting of its own: UpscalerFor makes it of the antialiasing the player picked.
//   Auto     AMD's FSR DLLs, through the 64-bit host beside the game (SecondRevisionHost64): FSR 4 where the card runs
//            it, FSR 3.1 elsewhere - and the built-in one while the host starts, or where it cannot run at all.
//   Builtin  the FSR 3.1 built into the renderer, in the game's own process.
//   DLSS     NVIDIA's DLSS through the same host, on an NVIDIA RTX card; where it cannot run the built-in FSR 3.1 does,
//            and the readout says why.
//   XeSS     Intel's XeSS through the same host, on any card it runs on (Shader Model 6.4: on Intel's Arc on its
//            matrix units, on the others by the DP4a path); where it cannot run the built-in FSR 3.1 does, and the
//            readout says why.
enum EUpscaler
{
    Upscaler_Auto = 0,
    Upscaler_Builtin,
    Upscaler_Dlss,
    Upscaler_Xess,
    Upscaler_Count
};

inline const char* const* UpscalerNames()
{
    static const char* const Names[Upscaler_Count] = { "Auto", "Builtin", "DLSS", "XeSS" };
    return Names;
}

template <typename TChar>
inline void UpscalerName(EUpscaler U, TChar* Out, int Max)
{
    const char* const N = UpscalerNames()[(U >= 0 && U < Upscaler_Count) ? (int)U : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// ---------------------------------------------------------------------------
// THE ANTIALIASING AS THE PLAYER PICKS IT: the AntiAliasing key and the menu's ANTIALIASING line (docs/nyitott-hibak.md
// 101.). One method, and under it the two settings that hang on it: UPSCALE (the Upscale key: how much smaller the
// scene is drawn under a temporal upscaler) and FRAMEGEN.
//   Auto   the temporal upscaler this card runs best: FSR4 on AMD's cards, DLSS on NVIDIA's, FSR3 on any other.
//   Off    none.
//   FSR3   the FSR 3.1 built into the renderer.
//   FSR4   AMD's FSR DLLs in the 64-bit host beside the game: FSR 4 where the card runs it. Not on an AMD card: the
//          built-in FSR 3.1 in its place, and COMPATIBLE AMD CHIP NEEDED.
//   DLSS   NVIDIA's DLSS in the same host. Not on an NVIDIA card: the built-in FSR 3.1, and NVIDIA CHIP NEEDED.
//   XeSS   Intel's XeSS in the same host, on any card with Shader Model 6.4. Where it will not run the host says why and
//          the built-in FSR 3.1 draws.
//   FXAA, SMAA, SSAA4X/9X/16X, MSAA2X/4X/8X/16X   as in EAntiAliasing.
// The temporal ones need this frame's motion vectors. What moves by itself gets vectors of its own whether the shadows
// are on or not (FBackend::SetMotionBodies), so none of them waits for the shadows.
// ---------------------------------------------------------------------------

enum EAaChoice
{
    AaChoice_Auto = 0,
    AaChoice_Off,
    AaChoice_Fsr3, AaChoice_Fsr4, AaChoice_Dlss, AaChoice_Xess,
    AaChoice_Fxaa, AaChoice_Smaa,
    AaChoice_Ssaa4, AaChoice_Ssaa9, AaChoice_Ssaa16,
    AaChoice_Msaa2, AaChoice_Msaa4, AaChoice_Msaa8, AaChoice_Msaa16,
    AaChoice_Count
};

// The names in the key's spelling, in the menu's order. One list, used both ways.
inline const char* const* AaChoiceNames()
{
    static const char* const Names[AaChoice_Count] =
    {
        "Auto", "Off", "FSR3", "FSR4", "DLSS", "XeSS", "FXAA", "SMAA",
        "SSAA4X", "SSAA9X", "SSAA16X", "MSAA2X", "MSAA4X", "MSAA8X", "MSAA16X"
    };
    return Names;
}

template <typename TChar>
inline void AaChoiceName(EAaChoice C, TChar* Out, int Max)
{
    const char* const N = AaChoiceNames()[(C >= 0 && C < AaChoice_Count) ? (int)C : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// The key's text; Auto - the default - for anything else, with bKnown false for the log.
template <typename TChar>
inline EAaChoice ParseAaChoice(const TChar* Text, bool& bKnown)
{
    const int q = NameIndex(Text, AaChoiceNames(), (int)AaChoice_Count);
    bKnown = q >= 0;
    return bKnown ? (EAaChoice)q : AaChoice_Auto;
}

// The menu's next choice, either way round.
inline EAaChoice AaChoiceStep(EAaChoice C, int Step)
{
    return (EAaChoice)((((int)C + Step) % (int)AaChoice_Count + (int)AaChoice_Count) % (int)AaChoice_Count);
}

// The choices a temporal upscaler runs: they draw with the jitter and need the motion vectors.
inline bool AaChoiceIsTemporal(EAaChoice C)
{
    return C == AaChoice_Auto || C == AaChoice_Fsr3 || C == AaChoice_Fsr4 || C == AaChoice_Dlss || C == AaChoice_Xess;
}

// The card's maker, from the adapter's PCI vendor id: what Auto picks by, and what FSR4 and DLSS need.
enum EGpuVendor
{
    Gpu_Other = 0,
    Gpu_Amd,
    Gpu_Nvidia,
    Gpu_Intel
};

inline EGpuVendor GpuVendorOf(unsigned VendorId)
{
    return (VendorId == 0x1002u) ? Gpu_Amd : ((VendorId == 0x10DEu) ? Gpu_Nvidia : ((VendorId == 0x8086u) ? Gpu_Intel
                                                                                                            : Gpu_Other));
}

// Where each vertex of a body that moves by itself was the frame before, for the motion vectors. Now and Before are the
// last two shapes made of it - x y z a vertex, each less the body's own place when it was made, so that only the
// shape's change is between them - Frames apart: one for a body near by, more for a far one whose shape is made only
// every few frames. WasAt is the body's place the frame before. A vertex of the later shape, moved back by one frame's
// share of the shape's change, at the body's old place:
//     Out = Now * (1 - 1 / Frames) + Before / Frames + WasAt
// With the shapes one frame apart that is the earlier shape where the body stood then - exact. Floats: three a vertex.
// Out is only written, never read: it may be memory that is slow to read back.
inline void MotionPrevPlaces(const float* Now, const float* Before, int Floats, int Frames, const float* WasAt,
                             float* Out)
{
    const float Old = 1.0f / (float)((Frames > 1) ? Frames : 1);
    const float New = 1.0f - Old;
    for (int i = 0; i + 2 < Floats; i += 3)
    {
        Out[i]     = Now[i]     * New + Before[i]     * Old + WasAt[0];
        Out[i + 1] = Now[i + 1] * New + Before[i + 1] * Old + WasAt[1];
        Out[i + 2] = Now[i + 2] * New + Before[i + 2] * Old + WasAt[2];
    }
}

// The GPU key: which of the machine's cards the renderer runs on. Auto, or nothing, leaves it to the order the cards
// come in - the system's "high performance first". A number is a card's place in that order, from 1, as the log lists
// them; anything else is a piece of a card's name, in either case ("rtx 5060", "intel") - and so is a number that is
// no card's place ("5060"). Names holds the cards in their order, Usable which of them can run the renderer at all.
// Returns the card asked for, or -1: nothing was asked (bAsked false), or what was asked names no card that can run -
// the caller then takes the first that can, and says so.
template <typename TChar, typename TName>
inline int GpuAskedFor(const TChar* Wanted, const TName* const* Names, const bool* Usable, int Count, bool& bAsked)
{
    bAsked = false;
    if (!Wanted)
        return -1;
    int B = 0;
    while (Wanted[B] == ' ' || Wanted[B] == '\t')
        B++;
    int E = B;
    while (Wanted[E])
        E++;
    while (E > B && (Wanted[E - 1] == ' ' || Wanted[E - 1] == '\t'))
        E--;
    const int Len = E - B;
    if (Len == 0)
        return -1;
    // The plain letters' lower case: all a card's name and the key's text may differ in.
    const auto Lower = [](unsigned C) { return (C >= 'A' && C <= 'Z') ? C + ('a' - 'A') : C; };
    const char* const Auto = "auto";
    bool bAuto = Len == 4;
    for (int k = 0; bAuto && k < 4; k++)
        bAuto = Lower((unsigned)Wanted[B + k]) == (unsigned)Auto[k];
    if (bAuto)
        return -1;
    bAsked = true;
    bool bNumber = Len <= 3;
    int  Number  = 0;
    for (int k = 0; bNumber && k < Len; k++)
    {
        const unsigned C = (unsigned)Wanted[B + k];
        bNumber = C >= '0' && C <= '9';
        Number  = Number * 10 + (int)(C - '0');
    }
    if (bNumber && Number >= 1 && Number <= Count)
        return Usable[Number - 1] ? Number - 1 : -1;
    for (int c = 0; c < Count; c++)
    {
        if (!Usable[c] || !Names[c])
            continue;
        const TName* const Name = Names[c];
        for (int s = 0; Name[s]; s++)
        {
            int k = 0;
            while (k < Len && Name[s + k] && Lower((unsigned)Name[s + k]) == Lower((unsigned)Wanted[B + k]))
                k++;
            if (k == Len)
                return c;
        }
    }
    return -1;
}

// What the choice comes to on this card: Auto's pick, and every other choice as it is.
inline EAaChoice AaChoiceOn(EAaChoice C, EGpuVendor V)
{
    if (C != AaChoice_Auto)
        return C;
    return (V == Gpu_Amd) ? AaChoice_Fsr4 : ((V == Gpu_Nvidia) ? AaChoice_Dlss : AaChoice_Fsr3);
}

// The mode the renderer runs: a temporal choice at the Upscale's size, and every other choice as it is.
inline EAntiAliasing AaRunFor(EAaChoice C, EUpscaling U)
{
    switch (C)
    {
    case AaChoice_Off:    return AA_Off;
    case AaChoice_Fxaa:   return AA_Fxaa;
    case AaChoice_Smaa:   return AA_Smaa;
    case AaChoice_Ssaa4:  return AA_Ssaa4;
    case AaChoice_Ssaa9:  return AA_Ssaa9;
    case AaChoice_Ssaa16: return AA_Ssaa16;
    case AaChoice_Msaa2:  return AA_Msaa2;
    case AaChoice_Msaa4:  return AA_Msaa4;
    case AaChoice_Msaa8:  return AA_Msaa8;
    case AaChoice_Msaa16: return AA_Msaa16;
    default:
        return AaFromUpscaling((U == Upscaling_Off) ? Upscaling_NativeAA : U);
    }
}

// Whose library does the temporal upscale: on an AMD card FSR4 is the host's FSR, and on an NVIDIA card DLSS is the
// host's DLSS; XeSS is the host's XeSS on any card. FSR4 or DLSS on another card, FSR3, and everything that does not
// upscale: the built-in FSR 3.1.
inline EUpscaler UpscalerFor(EAaChoice C, EGpuVendor V)
{
    const EAaChoice R = AaChoiceOn(C, V);
    if (R == AaChoice_Fsr4 && V == Gpu_Amd)
        return Upscaler_Auto;
    if (R == AaChoice_Dlss && V == Gpu_Nvidia)
        return Upscaler_Dlss;
    if (R == AaChoice_Xess)
        return Upscaler_Xess;
    return Upscaler_Builtin;
}

// Why a line that hangs on others cannot run, in the menu's and the readout's words (NeedText). Looked at in this
// order: the antialiasing the line needs, then the card.
enum ENeed
{
    Need_None = 0,
    Need_FsrOrDlss,
    Need_Fsr4OrDlss,
    Need_AmdChip,
    Need_NvidiaChip,
    Need_Count
};

inline const char* NeedText(ENeed N)
{
    switch (N)
    {
    case Need_FsrOrDlss:      return "NEEDS FSR OR DLSS OR XESS";
    case Need_Fsr4OrDlss:     return "NEEDS FSR4 OR DLSS";
    case Need_AmdChip:        return "COMPATIBLE AMD CHIP NEEDED";
    case Need_NvidiaChip:     return "NVIDIA CHIP NEEDED";
    default:                  return "";
    }
}

// The antialiasing itself: FSR4 and DLSS need their maker's card. Auto's pick is this card's own.
inline ENeed AaNeed(EAaChoice C, EGpuVendor V)
{
    if (!AaChoiceIsTemporal(C))
        return Need_None;
    const EAaChoice R = AaChoiceOn(C, V);
    if (R == AaChoice_Fsr4 && V != Gpu_Amd)
        return Need_AmdChip;
    if (R == AaChoice_Dlss && V != Gpu_Nvidia)
        return Need_NvidiaChip;
    return Need_None;
}

// UPSCALE, and the motion blur, which smears along the same vectors: a temporal upscaler.
inline ENeed VectorsNeed(EAaChoice C)
{
    return AaChoiceIsTemporal(C) ? Need_None : Need_FsrOrDlss;
}

// FRAMEGEN: the host makes the pictures with the library that upscales there - its FSR with FSR4 on an AMD card, its
// DLSS with DLSS on an NVIDIA one. Whether an NVIDIA card has DLSS's frame generation at all is the host's to say once
// it is up (FBackend::FrameGenWhy): the card's maker alone does not tell.
inline ENeed FrameGenNeed(EAaChoice C, EGpuVendor V)
{
    const EAaChoice R = AaChoiceOn(C, V);
    if (R == AaChoice_Dlss)
        return (V == Gpu_Nvidia) ? Need_None : Need_NvidiaChip;
    if (R != AaChoice_Fsr4)
        return Need_Fsr4OrDlss;
    return (V == Gpu_Amd) ? Need_None : Need_AmdChip;
}

// ---------------------------------------------------------------------------
// HDR AND TEN-BIT OUTPUT
//
// These are TWO different things, and the key keeps them apart on purpose.
//
//   Off        eight bits a channel, start to finish. What the renderer has always done.
//   10bitSDR   ten bits a channel, start to finish. THE SAME PICTURE at THE SAME BRIGHTNESS - only the steps
//              between neighbouring shades get four times finer. No display support is needed for it beyond a
//              ten-bit panel, and on an eight-bit one it still helps: the dithering the driver does then has
//              finer material to work from.
//   On         a wider range as well: the picture is anchored at paper white and whatever the lamps push above
//              it stays above it instead of being flattened to white. It RUNS on a display that is not in HDR
//              mode too - the swap chain is scRGB either way, the compositor then reads 1.0 as white, and the
//              final pass folds the highlights back in with a shoulder instead of letting them clip. So the
//              mode can be looked at, measured and judged anywhere, rather than turning itself off where
//              nobody can check it.
//
// WHY BIT DEPTH ALONE IS WORTH A MODE. The banding in this game does not come from the display, it comes from
// the brightness curve. The frame is stored in the scene target and then taken through out = in^Exponent, and
// with the game's usual brightness that exponent is about 0.4 - it STRETCHES the dark end. Two neighbouring
// eight-bit values near black, 10/255 and 11/255, come out about three eight-bit steps apart. That gap is the
// banding, and no amount of output precision closes it: it has to be the WHOLE chain that carries more bits,
// which is why the mode picks the scene target's format and not only the swap chain's.
//
// WHERE THE HEADROOM IN "On" COMES FROM. Not from an inverse tone map - pulling up whatever is already near
// white would light up a white wall exactly as brightly as a lamp, which is what makes most bolted-on HDR look
// wrong. It comes from the lamps themselves. The shadow compensation already turns the lamps up until the
// frame is as bright as it was before the shadows darkened it, and it is capped at ShadowBoostMax because
// beyond that the signs on the wall burn out against the 1.0 the format stops at. In HDR there is no such
// wall, so the cap moves to HdrBoostMax and the second stage - giving light back by weakening the shadows -
// is not needed at all. A wall under a lamp then genuinely ends up brighter than one across the room, out of
// the game's own light maps, with no guess about what counts as a light.
//
// The second source is the surfaces that carry their OWN brightness - see EmissiveBoost, which asks the engine's
// PF_Unlit flag rather than the finished picture.
// ---------------------------------------------------------------------------

enum EHdrMode
{
    Hdr_Off = 0,
    Hdr_Ten,        // "10bitSDR" in the ini - an identifier cannot start with a digit
    Hdr_On,
    Hdr_Count
};

inline const char* const* HdrNames()
{
    static const char* const Names[Hdr_Count] = { "Off", "10bitSDR", "On" };
    return Names;
}

template <typename TChar>
inline void HdrName(EHdrMode M, TChar* Out, int Max)
{
    const char* const N = HdrNames()[(M >= 0 && M < Hdr_Count) ? (int)M : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Off for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline EHdrMode ParseHdrMode(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return Hdr_Off;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    int E = B;
    while (Text[E])
        E++;
    while (E > B && (Text[E - 1] == ' ' || Text[E - 1] == '\t'))
        E--;
    const int Len = E - B;
    const char* const Spellings[Hdr_Count] = { "Off", "10bitSDR", "On" };
    for (int q = 0; q < (int)Hdr_Count; q++)
    {
        const char* N = Spellings[q];
        int k = 0;
        for (; k < Len && N[k]; k++)
        {
            unsigned C = (unsigned)Text[B + k];
            if (C >= 'A' && C <= 'Z')
                C += 'a' - 'A';
            unsigned D = (unsigned)N[k];
            if (D >= 'A' && D <= 'Z')
                D += 'a' - 'A';
            if (C != D)
                break;
        }
        if (k == Len && N[k] == 0)
        {
            bKnown = true;
            return (EHdrMode)q;
        }
    }
    return Hdr_Off;
}

// Bits a colour channel through the whole chain - the scene target, everything drawn into it, and the swap
// chain. Sixteen means half floats, which is what lets a value exceed one at all.
inline int HdrSceneBits(EHdrMode M)
{
    return (M == Hdr_On) ? 16 : ((M == Hdr_Ten) ? 10 : 8);
}

// Why the output runs below what was asked for - what the readout says under its OUTPUT line.
enum EHdrFallback
{
    HdrFallback_None = 0,
    HdrFallback_NeedsHdrDisplay,     // HDR asked, and the display cannot show it
    HdrFallback_HdrOffInWindows,     // HDR asked, and the display could - but Windows has it switched off
    HdrFallback_Needs10BitDisplay,   // ten bits asked, and the connection carries eight
    HdrFallback_Failed,              // the output transform could not be built on this machine
    HdrFallback_SafeMode,            // SafeMode: the display is not asked, the picture is eight-bit SDR
    HdrFallback_Count
};

// What the output can really run on the display the picture is on - the chain On -> 10bitSDR -> Off, a step down
// wherever the display cannot take a step. HDR only where the display is IN HDR MODE: on an SDR display the frame would
// be folded back under white by the compositor, which flattens every bright surface rather than showing it brighter.
// Ten bits only where the connection carries ten; DisplayBits unknown (0 or less) is taken as able, since a display that
// would not say is no reason to take away what may well work. The setting itself never changes - this is what RUNS,
// worked out again whenever the picture moves to another display.
inline EHdrMode HdrModeForDisplay(EHdrMode Asked, bool bDisplayInHdr, int DisplayHdrCapable, int DisplayBits,
                                  EHdrFallback& Why)
{
    Why = HdrFallback_None;
    EHdrMode M = (Asked >= Hdr_Off && Asked < Hdr_Count) ? Asked : Hdr_Off;
    if (M == Hdr_On && !bDisplayInHdr)
    {
        Why = (DisplayHdrCapable > 0) ? HdrFallback_HdrOffInWindows : HdrFallback_NeedsHdrDisplay;
        M   = Hdr_Ten;
    }
    if (M == Hdr_Ten && DisplayBits > 0 && DisplayBits < 10)
    {
        if (Why == HdrFallback_None)
            Why = HdrFallback_Needs10BitDisplay;
        M = Hdr_Off;
    }
    return M;
}

// How far the shadow compensation may turn the lamps up. In SDR it stops at ShadowBoostMax because the signs on
// the wall burn out above it; in HDR nothing burns out, so what sets the ceiling instead is when a DIFFUSE
// surface stops looking lit and starts looking self-lit - which is somewhere around two to three times paper
// white. Taken at the lower end: this multiplies the light maps, and a wall directly under a lamp is already
// near the top of the range before any of it is applied.
static const float HdrBoostMax = 2.0f;

inline float ShadowBoostCeiling(EHdrMode M)
{
    return (M == Hdr_On) ? HdrBoostMax : ShadowBoostMax;
}

// Stage two of the compensation - giving light back by WEAKENING the shadows - exists only because SDR runs
// out of range. It is a loss of contrast, which is the one thing HDR is for, so in HDR the shadows keep their
// full strength and the lamps do all the work.
inline float ShadowStrengthFloorFor(EHdrMode M)
{
    return (M == Hdr_On) ? 1.0f : ShadowStrengthFloor;
}

// WHAT IS ALLOWED TO GO ABOVE PAPER WHITE, and how the engine is asked rather than guessed at.
//
// The wrong way round is to look at the finished picture and pull up whatever is already near white. A white wall
// then lights up exactly as brightly as a lamp, and that is what makes most bolted-on HDR look wrong.
//
// The engine already knows. PF_Unlit means "this surface carries its own brightness": screens, signs, neon, the
// lit panels of a lift. It is the flag the renderer ALREADY reads to decide that a surface must not be darkened by
// a light map (WantsLightMap), so nothing new has to be worked out about what a surface is - the same answer is
// used for one more thing.
//
// FakeBackdrop - the sky - is deliberately NOT in here although it carries its own brightness too. The sky in this
// game is a painted backdrop, and lifting it would turn a night sky into a light source.
//
// Two, where a physically-minded guess would say eight. The scene is stored in the engine's own display space
// and goes out through out = in^Exponent and then a decode, so a factor of two here arrives as roughly 1.8 in
// linear light - already well clear of paper white, which is where the lamp separates from the wall it is on.
// Eight would put a computer screen six times brighter than the room it is in, which is not what this game
// looks like.
static const float HdrEmissiveBoost = 2.0f;

// One for everything else, so the caller can multiply unconditionally, and one in every mode but Hdr_On - the two
// SDR modes have no room above white to put it in, and a surface pushed there would only clip.
//
// OPAQUE ONLY, and that is not caution but arithmetic. A translucent draw blends with ONE, INV_SRC_COLOR - the
// result is src + dst * (1 - src) - and on a float target that factor is not clamped, so a source above one
// SUBTRACTS what is behind it. A corona would darken the wall it sits on.
//
// Coronas and muzzle flashes are exactly what wants lifting most, so this is the first thing to come back to.
// The way in is to submit such a run TWICE - once as it is, once purely additive at (boost - 1) - which sums to
// the wanted result with no negative factor anywhere.
//
// WHICH translucent draws count as light is the harder half, and three signals were measured and all three are
// wrong:
//
//   PF_Unlit                          is BACKWARDS. On a night street the SMOKE carries it and the corona does
//                                     not - so it would light up the smoke and leave the lamp dark.
//   "a tile before the HUD starts"    a corona arrives at Z = 1, which is where the HUD lives (IsHudTile), so
//                                     the corona is itself what ends the scene.
//   "translucent at Z <= 1, before"   in a level with no coronas the first such tile is the HUD's own
//                                     translucent background.
//
// What is left is the texture's NAME, which this game does keep to - but that is a rule about Deus Ex rather
// than about the engine, and it is not one to adopt quietly. Nothing is boosted here until it is chosen
// deliberately: the price of guessing is glowing smoke or a glowing HUD. docs/hdr-mod.md has the measurements.
inline float EmissiveBoost(EHdrMode Mode, unsigned int PolyFlags)
{
    if (Mode != Hdr_On)
        return 1.0f;
    if (BlendFor(PolyFlags) != Blend_Opaque)
        return 1.0f;
    return (PolyFlags & RDX_PF_Unlit) ? HdrEmissiveBoost : 1.0f;
}

// ---------------------------------------------------------------------------
// The output transform, for HDR: the maths the final pass runs per pixel, here so it can be checked on the
// CPU against the same numbers the shader uses.
// ---------------------------------------------------------------------------

// scRGB (DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709), the format the swap chain is in under Hdr_On: linear light,
// Rec.709 primaries, and 1.0 means 80 nits BY DEFINITION. So a picture whose white is meant to read at
// PaperWhiteNits comes out multiplied by this.
static const float ScRgbUnitNits = 80.0f;

// Where the frame's own white sits. Windows puts SDR content at 200 nits by default in HDR mode, and 100 to 200
// is the usual range for a game's paper white; 200 keeps this one as bright beside everything else on the
// desktop as it is now, which is the half of the job that says the picture must not change.
static const float HdrPaperWhiteNits = 200.0f;

inline float PaperWhiteScale(float Nits)
{
    if (!(Nits > 0.0f))
        Nits = HdrPaperWhiteNits;
    return Nits / ScRgbUnitNits;
}

// The shoulder that keeps a bright pixel from being cut off flat at the display's ceiling.
//
// It is IDENTITY below the knee. That is the whole design: the diffuse picture - which is everything the
// compensation anchors at paper white, so nearly the whole frame - passes through untouched, and only what
// the lamps pushed above the knee is bent. A tone curve that starts at zero would change every pixel in the
// game, and the point of this mode is that it does not.
//
// Above the knee: K + (M - K) * (1 - exp(-(x - K) / (M - K))). It meets the identity line at the knee with
// slope one, so there is no visible crease there, and it approaches M without ever reaching it, so nothing
// clips however bright it gets.
// Where the shoulder starts, as a fraction of the display's white. Everything below it is untouched, so this
// is also the promise: three quarters of the range is the picture exactly as it was, and the top quarter is
// where a highlight is allowed to be bent rather than cut off. Low enough to leave room to bend in, high
// enough that the walls - which the compensation anchors well below white - never reach it.
static const float HdrShoulderKnee = 0.75f;

inline float HighlightShoulder(float X, float Knee, float Max)
{
    if (!(X > Knee))
        return X;
    // A ceiling at or below the knee leaves nothing to bend into, so the value is simply not allowed past it.
    // It cannot happen with the numbers the final pass passes in, but the shader's own version says min(X,
    // Ceiling) here and the two must not disagree about anything, reachable or not.
    if (!(Max > Knee))
        return (X < Max) ? X : Max;
    const float Span = Max - Knee;
    const float T    = (X - Knee) / Span;
    // The shader has exp() in hardware; this side has no <cmath>, so the exponent is worked out by halving T
    // until the series converges quickly and squaring the result back up. The series ALONE is not enough: its
    // terms grow before they shrink, and at T = 40 it diverges into nonsense - which is exactly the range a
    // bright lamp lands in.
    float E;
    {
        int   Halvings = 0;
        float R        = T;
        while (R > 0.5f && Halvings < 24)
        {
            R *= 0.5f;
            Halvings++;
        }
        E = 1.0f;
        float Term = 1.0f;
        for (int k = 1; k <= 8; k++)
        {
            Term *= -R / (float)k;
            E    += Term;
        }
        for (int k = 0; k < Halvings; k++)
            E *= E;
        if (!(E > 0.0f))
            E = 0.0f;
        if (E > 1.0f)
            E = 1.0f;
    }
    return Knee + Span * (1.0f - E);
}

// The scene is stored the way this renderer has always stored it - the engine's own gamma space, where the
// numbers are already display-ready - so going to scRGB's LINEAR light means undoing that encoding. 2.2 and
// not the sRGB piecewise curve: 2.2 is what the rest of the renderer assumes (FFinalPass's box filter mixes in
// it), and mixing two definitions of the same thing in one pipeline is how a picture ends up subtly wrong in
// the shadows with nobody able to say why.
static const float HdrDecodeGamma = 2.2f;

// What the false-colour view paints, so the bands are one list and the shader and the tests cannot disagree.
// Band 0 is below paper white, and everything in the game should be in it apart from the lamps.
enum EHdrBand
{
    HdrBand_Below = 0,   // < 1x paper white      blue
    HdrBand_One,         // 1x .. 2x              green
    HdrBand_Two,         // 2x .. 4x              yellow
    HdrBand_Four,        // > 4x                  red
    HdrBand_Count
};

inline EHdrBand HdrBandOf(float Ratio)
{
    if (Ratio < 1.0f)  return HdrBand_Below;
    if (Ratio < 2.0f)  return HdrBand_One;
    if (Ratio < 4.0f)  return HdrBand_Two;
    return HdrBand_Four;
}

// ---------------------------------------------------------------------------
// Reading a scene target back on the CPU (FBackend::ReadScene - the engine's screenshots and the measuring
// scripts). The target's format now follows the HDR mode, so the unpacking does too.
// ---------------------------------------------------------------------------

// R10G10B10A2_UNORM: one 32-bit word a texel, red in the lowest ten bits. Out is 0..1.
inline void UnpackTenBit(unsigned int Word, float& R, float& G, float& B)
{
    R = (float)( Word        & 0x3FFu) * (1.0f / 1023.0f);
    G = (float)((Word >> 10) & 0x3FFu) * (1.0f / 1023.0f);
    B = (float)((Word >> 20) & 0x3FFu) * (1.0f / 1023.0f);
}

// A half float to a single one. Subnormals and infinities are handled because a render target really does
// produce them: an infinity in one pixel that turned into a wild number would show up as a lone bright dot in
// a screenshot and look like a rendering fault.
inline float HalfToFloat(unsigned short H)
{
    const unsigned int Sign     = (unsigned int)(H >> 15) & 1u;
    const unsigned int Exponent = (unsigned int)(H >> 10) & 0x1Fu;
    const unsigned int Mantissa = (unsigned int)H & 0x3FFu;
    float V;
    if (Exponent == 0)
    {
        // Zero or subnormal: the mantissa is a plain fraction of 2^-14.
        V = (float)Mantissa * (1.0f / 1024.0f) * (1.0f / 16384.0f);
    }
    else if (Exponent == 31)
    {
        // Infinity or not-a-number. Half floats stop at 65504, so the largest finite value is the honest
        // answer for a reader that has to hand back a number.
        V = 65504.0f;
    }
    else
    {
        float Scale = 1.0f;
        int   E     = (int)Exponent - 15;
        for (; E > 0; E--)
            Scale *= 2.0f;
        for (; E < 0; E++)
            Scale *= 0.5f;
        V = (1.0f + (float)Mantissa * (1.0f / 1024.0f)) * Scale;
    }
    return Sign ? -V : V;
}

// A float's bits as the shaders' uint slots take them (ffx_a.h's AU1_AF1) - x86 is little-endian.
inline unsigned int FloatBits(float F)
{
    const unsigned char* const B = (const unsigned char*)&F;
    return (unsigned int)B[0] | ((unsigned int)B[1] << 8) | ((unsigned int)B[2] << 16) | ((unsigned int)B[3] << 24);
}

// The jitter a temporal upscaler (FSR 2/3) needs: the picture is drawn a fraction of a pixel off each frame, along the
// Halton(2, 3) sequence - the same one ffxFsr3UpscalerGetJitterOffset hands out. The result is in pixels, -0.5 to 0.5.
inline float HaltonAt(int Index, int Base)
{
    float F = 1.0f, R = 0.0f;
    int   I = (Index > 0) ? Index : 0;
    while (I > 0)
    {
        F /= (float)Base;
        R += F * (float)(I % Base);
        I /= Base;
    }
    return R;
}

// Index counts the frames; the sequence repeats after PhaseCount of them. The SDK counts its phases from one.
inline void FsrJitterOffset(int Index, int PhaseCount, float& X, float& Y)
{
    const int N = (PhaseCount > 0) ? PhaseCount : 1;
    const int I = ((Index % N) + N) % N + 1;
    X = HaltonAt(I, 2) - 0.5f;
    Y = HaltonAt(I, 3) - 0.5f;
}

// How many phases the sequence has: eight times the square of the scale, cut to a whole number - the SDK's own rule
// (1.5x: 18, 1.7x: 23, 2x: 32, 3x: 72).
inline int FsrJitterPhaseCount(int RenderWidth, int DisplayWidth)
{
    if (RenderWidth <= 0 || DisplayWidth <= 0)
        return 1;
    const float Ratio = (float)DisplayWidth / (float)RenderWidth;
    const int   N     = (int)(8.0f * Ratio * Ratio);
    return (N < 1) ? 1 : N;
}

// Which of the level's polygons go into a light's world map: all but the zone portals, which are no surface at all. The
// map is to answer whether the level's own geometry stood between a surface and the light when its light maps were made.
// Which surfaces the light maps' tracer let through is not measured yet - and a wall wrongly left out would put an actor's
// shadow onto a spot its light map has darkened already (twice dark), while one wrongly kept only loses a shadow there.
inline bool BspBlocksLight(unsigned int PolyFlags)
{
    return (PolyFlags & RDX_PF_Portal) == 0;
}

// Which of a mesh's faces go into the actors' maps: not the invisible ones (a weapon's attachment faces), not the
// translucent or modulated ones (glass, glow). The masked ones do, through their texture's mask (MeshFaceMasked): a plant's
// leaves cast their own shape, not whole rectangles.
inline bool MeshFaceCastsShadow(unsigned int PolyFlags)
{
    return (PolyFlags & (RDX_PF_Invisible | RDX_PF_Translucent | RDX_PF_Modulated)) == 0;
}

inline bool MeshFaceMasked(unsigned int PolyFlags)
{
    return (PolyFlags & RDX_PF_Masked) != 0;
}

// The masked casters' masks (FShadowMaps, RenderDevice.cpp): a texture's texels as opaque or not, point-sampled onto a square
// of ShadowMaskSize, in the maps' own texture array of ShadowMaxMasks - slot 0 solid, for every face without a mask.
enum { ShadowMaskSize = 256, ShadowMaxMasks = 64 };

// A DXT1 block's texel (X, Y within the 4x4 block): transparent where the block's first endpoint is not above its second and
// the texel's index is 3 - the format's one-bit alpha. Block: 8 bytes - the two RGB565 endpoints, little-endian, then 32
// index bits, two a texel, row by row.
inline bool Dxt1TexelTransparent(const unsigned char* Block, int X, int Y)
{
    const unsigned C0 = (unsigned)Block[0] | ((unsigned)Block[1] << 8);
    const unsigned C1 = (unsigned)Block[2] | ((unsigned)Block[3] << 8);
    if (C0 > C1)
        return false;
    const unsigned Bits = (unsigned)Block[4] | ((unsigned)Block[5] << 8) | ((unsigned)Block[6] << 16) |
                          ((unsigned)Block[7] << 24);
    return ((Bits >> (2 * (Y * 4 + X))) & 3u) == 3u;
}

// The plan's rule for the same question (docs/terv-latvany-roadmap.md 3.2), for an A/B behind the marker
// SecondRevision-shadow-translucent.txt until measured: the see-through surfaces - masked, translucent, invisible, modulated
// (PF_NoOcclude), unless the level marks them PF_Occlude - and the sky are left out as well. The code review of:
// with every polygon kept, a fence takes the actor shadows behind it, and a glass globe around a lamp all of its shadows.
inline bool BspBlocksLightSeeThrough(unsigned int PolyFlags)
{
    if ((PolyFlags & (RDX_PF_Portal | RDX_PF_FakeBackdrop)) != 0)
        return false;
    const unsigned int NoOcclude = RDX_PF_Masked | RDX_PF_Translucent | RDX_PF_Invisible | RDX_PF_Modulated;
    return (PolyFlags & NoOcclude) == 0 || (PolyFlags & RDX_PF_Occlude) != 0;
}

// A light's world map at the start of a frame (FShadowMaps::Render). Fresh: drawn from where the light is, within a unit.
// Stale: the light moved a little since, or its reach shrank - the map serves as it is until its turn comes. Unusable:
// moved further, or its reach grew past what the map holds - the light waits out of the lookup until the map is drawn
// again. Missing: none drawn for this light yet.
enum EWorldMapState
{
    WorldMap_Fresh = 0,
    WorldMap_Stale,
    WorldMap_Unusable,
    WorldMap_Missing,
};

// How far a light may stand from where its world map was drawn with the map still used, engine units. An estimate, not
// measured: the lookup's own slack against the world maps is a few units at a few metres (FinalPass.cpp), and a lamp
// carried at a run moves some two units a frame - so a map a few frames old still serves while the budget draws the
// missing ones first. The code review of: two lights moving every frame - the flashlight's beams - took both
// redraws of every frame, and every other new light waited until the player stood still.
const float ShadowStaleTolerance = 8.0f;

inline EWorldMapState ClassifyWorldMap(bool bHasMap, const FVec3& DrawnAt, float DrawnRadius, const FVec3& Now, float Radius)
{
    if (!bHasMap)
        return WorldMap_Missing;
    const float DX = Now.X - DrawnAt.X, DY = Now.Y - DrawnAt.Y, DZ = Now.Z - DrawnAt.Z;
    const float AX = (DX < 0.0f) ? -DX : DX, AY = (DY < 0.0f) ? -DY : DY, AZ = (DZ < 0.0f) ? -DZ : DZ;
    const float Moved = (AX > AY) ? ((AX > AZ) ? AX : AZ) : ((AY > AZ) ? AY : AZ);
    const float Grew  = Radius - DrawnRadius;
    if (Moved > ShadowStaleTolerance || Grew > ShadowStaleTolerance)
        return WorldMap_Unusable;
    if (Moved > 1.0f || Grew > 1.0f || Grew < -1.0f)
        return WorldMap_Stale;
    return WorldMap_Fresh;
}

// Which world maps a frame draws, at most Budget: the missing ones first, in the pick's order (the nearest lights first),
// then the unusable ones, then the stale ones - each of the two the oldest first (Age: frames since drawn; equal ages keep
// the pick's order). A fresh map is never drawn again. The lights' indices go to Out; returns how many.
inline int WorldRedrawOrder(const EWorldMapState* State, const int* Age, int Count, int Budget, int* Out)
{
    int N = 0;
    for (int i = 0; i < Count && N < Budget; i++)
        if (State[i] == WorldMap_Missing)
            Out[N++] = i;
    for (int Pass = 0; Pass < 2 && N < Budget; Pass++)
    {
        const EWorldMapState Want  = (Pass == 0) ? WorldMap_Unusable : WorldMap_Stale;
        const int            Start = N;
        for (int i = 0; i < Count; i++)
        {
            if (State[i] != Want)
                continue;
            int Pos = N;
            while (Pos > Start && Age[Out[Pos - 1]] < Age[i])
                Pos--;
            if (Pos >= Budget)
                continue;
            const int Last = (N < Budget) ? N : Budget - 1;
            for (int k = Last; k > Pos; k--)
                Out[k] = Out[k - 1];
            Out[Pos] = i;
            if (N < Budget)
                N++;
        }
    }
    return N;
}

// A scene node's view for the shadows' lookup (RenderDevice.cpp): its part of the output, 0..1 - x, y, width, height - and
// its half-angle tangents. The engine projects a point to Point.X * Proj.Z / Point.Z + FX15 (Engine\Inc\UnRender.h,
// FTransform::Project), so its half width FX2 lies at the tangent FX2 / Proj.Z, and the height's the same way. False when
// the node, the output or the projection has no size.
inline bool FrameShadowView(int XB, int YB, int X, int Y, int OutW, int OutH, float FX2, float FY2, float ProjZ,
                            float Rect[4], float& TanX, float& TanY)
{
    if (X <= 0 || Y <= 0 || OutW <= 0 || OutH <= 0 || !(ProjZ > 0.0f) || !(FX2 > 0.0f) || !(FY2 > 0.0f))
        return false;
    Rect[0] = (float)XB / (float)OutW;
    Rect[1] = (float)YB / (float)OutH;
    Rect[2] = (float)X / (float)OutW;
    Rect[3] = (float)Y / (float)OutH;
    TanX    = FX2 / ProjZ;
    TanY    = FY2 / ProjZ;
    return true;
}

// Whether a caster's shadow from a light can reach the view at all. The right question is not where the LIGHT is but where its SHADOW lands: a shadow runs from the caster
// straight away from the light, as far as the light still reaches, and if no part of that stretch touches the view,
// nothing it darkens can be seen. A lamp one floor down throws its shadows onto that floor; a lamp behind the player
// throws the player's shadow forward, into plain sight. The same test settles both.
//
// Everything is in CAMERA space (X right, Y down, Z forward), as SphereTouchesView wants it. The stretch is covered by
// Segments spheres, each enclosing its own piece plus the caster's radius, so the answer is CONSERVATIVE: it never says
// "cannot be seen" about a shadow that could be - and the other way round only costs one needless draw.
inline bool ShadowCanReachView(const FVec3& LightCam, const FVec3& CasterCam, float CasterRadius, float LightRadius,
                               float TanX, float TanY, int Segments)
{
    const float DX = CasterCam.X - LightCam.X, DY = CasterCam.Y - LightCam.Y, DZ = CasterCam.Z - LightCam.Z;
    const float D2 = DX * DX + DY * DY + DZ * DZ;
    // The caster sits on the light: there is no shadow direction, so decide nothing and keep it.
    if (!(D2 > 1.0e-6f))
        return true;
    const float Dist = Sqrt(D2);
    // Past the light's reach there is no shadow to cast.
    if (Dist >= LightRadius)
        return false;
    const float Len = LightRadius - Dist;          // how far beyond the caster the shadow can still fall
    const float AX = DX / Dist, AY = DY / Dist, AZ = DZ / Dist;
    const int   N  = (Segments < 1) ? 1 : ((Segments > 8) ? 8 : Segments);
    const float Step = Len / (float)N;
    for (int k = 0; k < N; k++)
    {
        const float Mid = ((float)k + 0.5f) * Step;
        if (SphereTouchesView(CasterCam.X + AX * Mid, CasterCam.Y + AY * Mid, CasterCam.Z + AZ * Mid,
                              0.5f * Step + CasterRadius, TanX, TanY))
            return true;
    }
    return false;
}

// A light the shadow pick may choose (RenderDevice.cpp).
struct FShadowLightChoice
{
    float Distance;      // from the eye to the light, engine units
    float Radius;        // its reach (WorldLightRadius)
    bool  bHasCaster;    // a caster stands within its sphere - without one it has no shadow to give
    bool  bWasChosen;    // chosen the frame before
};

// The shadows' lights: those with a caster whose sphere comes within Reach of the eye - the nearest first, at most Max.
// A light chosen the frame before counts as nearer by the factor Keep (below 1), for its place and for Reach alike: at
// the edge of the pick the choice would flip frame to frame otherwise, and every flip redraws a light's world map. The
// chosen indices go to Out, nearest first; returns how many.
//
// THE PICK IS ALL ROUND, and that is a point of principle rather than a saving. Requiring a light's sphere to touch
// the view looks like the obvious cull and is wrong: a lamp BEHIND the player throws the player's shadow FORWARD,
// into plain sight, while its own sphere never touches the view. Culled that way, walking a corridor makes shadows
// appear and vanish in a blink as lights enter and leave the view.
// Being all round costs one distance comparison a light on the CPU; what costs is drawing the maps of whatever is
// chosen, and that is what Max is for.
//
// OutNearestRejected (optional): the TRUE distance of the nearest light the pick turned away - one that did not fit, or
// one a newcomer pushed out; either is the light the pick would take next. ShadowLightWeight fades the graded band out
// towards it, so the light leaving and the light arriving both cross at a weight of zero. A huge value when none was.
inline int PickShadowLights(const FShadowLightChoice* C, int Count, int Max, float Reach, float Keep, int* Out,
                            float* OutNearestRejected = nullptr)
{
    enum { Cap = ShadowMaxLights };
    float Score[Cap];
    if (Max > Cap)
        Max = Cap;
    float Rejected = 3.0e38f;
    int N = 0;
    for (int i = 0; i < Count; i++)
    {
        const FShadowLightChoice& L = C[i];
        // The code review of: at the reach's edge the choice flipped as the eye crossed it.
        const float Beyond = L.bWasChosen ? (L.Distance - L.Radius) * Keep : L.Distance - L.Radius;
        if (!L.bHasCaster || Beyond > Reach)
            continue;
        const float S = L.bWasChosen ? L.Distance * Keep : L.Distance;
        int Pos = N;
        while (Pos > 0 && Score[Pos - 1] > S)
            Pos--;
        if (Pos >= Max)
        {
            if (L.Distance < Rejected)
                Rejected = L.Distance;
            continue;
        }
        if (N == Max && C[Out[Max - 1]].Distance < Rejected)      // the last one falls out - read before the shift
            Rejected = C[Out[Max - 1]].Distance;
        const int Last = (N < Max) ? N : Max - 1;
        for (int k = Last; k > Pos; k--)
        {
            Score[k] = Score[k - 1];
            Out[k]   = Out[k - 1];
        }
        Score[Pos] = S;
        Out[Pos]   = i;
        if (N < Max)
            N++;
    }
    if (OutNearestRejected)
        *OutNearestRejected = Rejected;
    return N;
}

// How near a light is, for the shadow pick: the vertical counted Vertical times, so the eye scans an
// oblate spheroid instead of a sphere and the lights on its own floor come first. A light above is on the floor above
// more often than not and throws a poor shadow anyway; one below is on a floor that does not matter. UE1's Z is up.
// The weight (ShadowLightWeight) takes this same distance, not the true one - the pick and the fade must measure alike,
// or a light could enter the pick at its edge and still get full strength: on a staircase, exactly where the vertical
// weighting matters (agreed ). The light's own falloff stays true: the shader works that out per pixel.
inline float WeightedLightDistance(float DX, float DY, float DZ, float Vertical)
{
    const float VZ = DZ * ((Vertical > 0.0f) ? Vertical : 1.0f);
    return Sqrt(DX * DX + DY * DY + VZ * VZ);
}

// ShadowLightWeight's Full: the FullLights-th nearest picked light's distance, or a huge value when the pick holds no more
// than FullLights lights - then there is no graded band. Picked comes in the pick's order, where a kept light counts
// nearer than it is (Keep), so the true distances are sorted again here.
inline float GradedFullDistance(const FShadowLightChoice* C, const int* Picked, int PickCount, int FullLights)
{
    enum { Cap = ShadowMaxLights };
    if (FullLights <= 0 || PickCount <= FullLights)
        return 3.0e38f;
    if (PickCount > Cap)
        PickCount = Cap;
    float D[Cap];
    for (int k = 0; k < PickCount; k++)
    {
        const float V = C[Picked[k]].Distance;
        int Pos = k;
        while (Pos > 0 && D[Pos - 1] > V)
        {
            D[Pos] = D[Pos - 1];
            Pos--;
        }
        D[Pos] = V;
    }
    return D[FullLights - 1];               // PickCount > FullLights, so this is inside
}

// A picked light's target weight, 0..1: the nearest FullLights at full strength, the rest fading
// with their distance. Two fades, and the smaller wins:
// - BY RANK, from Full (GradedFullDistance) down to zero at End - the nearest light the pick turned away
// (PickShadowLights' OutNearestRejected), or the light's own reach edge when none was. The light LEAVING the pick is
// the one crossing End, so it leaves at zero, and the one taking its place enters there too: no jump.
// - BY REACH, over the last ReachFade before ReachEdge (the reach plus the light's radius, where the pick lets it go):
// every light fades out there, graded or not. Outdoors, with fewer lights than FullLights, there is no graded band
// at all, and a floodlight would otherwise vanish at full strength the moment it crossed the reach.
// the worked example - 1,1,1,2,3 | 5,6,9,12,17 metres, the eleventh light at 20 - comes out 88, 82, 65, 47 and 18%,
// which is the shape the fade is meant to have: nearly full at the front of the band, near nothing at its end. All
// distances are WeightedLightDistance's. What is SHOWN is this followed over
// time (FollowOverTime): the keep margin can still move End by a frame, and a light entering always starts from zero.
inline float ShadowLightWeight(float Dist, float Full, float End, float ReachEdge, float ReachFade)
{
    float W = 1.0f;
    if (Dist > Full)
    {
        if (Dist >= End)
            W = 0.0f;
        else
            W = (End - Dist) / (End - Full);   // End > Dist > Full here: never a division by zero
    }
    if (ReachFade > 0.0f)
    {
        float R = (ReachEdge - Dist) / ReachFade;
        R = (R < 0.0f) ? 0.0f : ((R > 1.0f) ? 1.0f : R);
        if (R < W)
            W = R;
    }
    return W;
}

// ---------------------------------------------------------------------------
// Ambient occlusion: how shut in a surface is by what is near it - the crease where a wall meets the floor, the ground at
// the foot of a crate, the underside of a desk - and so how much less it gets of the light that arrives from everywhere
// at once. The game's own lighting has no such thing. Its ambient light is one flat number a zone, the same in the
// middle of a floor as in the corner beside it, which is much of why a room of this age looks flat, and why a crate
// seems to hover a finger's width above the floor it stands on.
//
// For now it is worked out from the frame's depth alone (SSAO - FinalPass, GFinalAoShader), and it darkens the shadow
// FACTOR, not the picture. That is the whole of the design. The factor is what the compensation measures
// (FBackend::SampleShadowMetric), so the compensation takes the occlusion into account exactly as it takes the shadows,
// and stays the last step of the light: a crease comes out darker than the open floor beside it, and the picture as a
// whole keeps its brightness. It also means the occlusion runs only with the shadows - without them there is no factor.
// ---------------------------------------------------------------------------

enum EAmbientOcclusion
{
    Ao_Off = 0,
    Ao_Ssao,
    Ao_Rt,      // the SSAO's raw pass traced with rays (docs/terv-rt.md) - on a card that traces rays; elsewhere the SSAO
    Ao_Count
};

inline const char* const* AoNames()
{
    static const char* const Names[Ao_Count] = { "Off", "SSAO", "RT" };
    return Names;
}

template <typename TChar>
inline void AoName(EAmbientOcclusion A, TChar* Out, int Max)
{
    const char* const N = AoNames()[(A >= 0 && A < Ao_Count) ? (int)A : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Off for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline EAmbientOcclusion ParseAmbientOcclusion(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return Ao_Off;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    int E = B;
    while (Text[E])
        E++;
    while (E > B && (Text[E - 1] == ' ' || Text[E - 1] == '\t'))
        E--;
    const int Len = E - B;
    const char* const Spellings[Ao_Count] = { "Off", "SSAO", "RT" };
    for (int q = 0; q < (int)Ao_Count; q++)
    {
        const char* N = Spellings[q];
        int k = 0;
        for (; k < Len && N[k]; k++)
        {
            unsigned C = (unsigned)Text[B + k];
            if (C >= 'A' && C <= 'Z')
                C += 'a' - 'A';
            unsigned D = (unsigned)N[k];
            if (D >= 'A' && D <= 'Z')
                D += 'a' - 'A';
            if (C != D)
                break;
        }
        if (k == Len && N[k] == 0)
        {
            bKnown = true;
            return (EAmbientOcclusion)q;
        }
    }
    return Ao_Off;
}

// ---------------------------------------------------------------------------
// The readout on screen (DebugReadout=Off/FPS/On)
//
// Off shows nothing. FPS shows the first block alone - the frame rate limit and the frame rate, which is what anyone
// wants at a glance. On shows every block: that one with the video memory under it, then what the screen and the
// output are, then what the picture is drawn with (rays, antialiasing, shadows, occlusion, water), then what the
// shadows cost the picture as a whole. A blank line between the blocks, so the eye finds the one it came for.
// ---------------------------------------------------------------------------

enum EReadout
{
    Readout_Off = 0,
    Readout_Fps,        // the frame rate block alone
    Readout_On,         // every block
    Readout_Count
};

inline const char* const* ReadoutNames()
{
    static const char* const Names[Readout_Count] = { "Off", "FPS", "On" };
    return Names;
}

template <typename TChar>
inline void ReadoutName(EReadout R, TChar* Out, int Max)
{
    const char* const N = ReadoutNames()[(R >= 0 && R < Readout_Count) ? (int)R : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Off for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline EReadout ParseReadout(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return Readout_Off;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    int E = B;
    while (Text[E])
        E++;
    while (E > B && (Text[E - 1] == ' ' || Text[E - 1] == '\t'))
        E--;
    const int Len = E - B;
    const char* const Spellings[Readout_Count] = { "Off", "FPS", "On" };
    for (int q = 0; q < (int)Readout_Count; q++)
    {
        const char* N = Spellings[q];
        int k = 0;
        for (; k < Len && N[k]; k++)
        {
            unsigned C = (unsigned)Text[B + k];
            if (C >= 'A' && C <= 'Z')
                C += 'a' - 'A';
            unsigned D = (unsigned)N[k];
            if (D >= 'A' && D <= 'Z')
                D += 'a' - 'A';
            if (C != D)
                break;
        }
        if (k == Len && N[k] == 0)
        {
            bKnown = true;
            return (EReadout)q;
        }
    }
    return Readout_Off;
}

// The menu steps Off, FPS, On and round again.
inline EReadout ReadoutStep(EReadout R, int Step)
{
    const int N = (int)Readout_Count;
    return (EReadout)((((int)R + Step) % N + N) % N);
}

// ---------------------------------------------------------------------------
// Water (WaterQuality=Off/Low/Medium/High/RT, docs/meres-11)
//
// Off is the engine's own water. Low draws a see-through water surface over a copy of what is behind it: it wavers with
// gentle waves, deepens towards the water zone's own colour, and its texture fades in at the shoreline. Medium adds the
// mirror image, followed through the picture, and the lamps' glints. High adds the caustics: the net of light the waves
// throw on what lies under the water. RT is High with the mirror image and the glints from rays traced against the
// scene (docs/terv-rt.md).
// ---------------------------------------------------------------------------

// How the relief is drawn (Geometry): not at all; as the POM's illusion inside the flat wall; or as real geometry the
// GPU raises out of the wall from the same height maps (displacement mapping: the wall is tessellated and its points
// are moved). Both read the same maps, the same list and the same depth number, and their strength is meant to
// match: the POM carves the relief's depth into the wall, the displacement raises it out by the same depth.
enum EGeometry
{
    Geometry_Off = 0,
    Geometry_Pom,
    Geometry_Tess,
    Geometry_Count
};

// The longest of these with its closing zero: what a buffer for a name has to hold.
enum { GeometryNameChars = 24 };

inline const char* const* GeometryNames()
{
    static const char* const Names[Geometry_Count] = { "Off", "POM", "DisplacementMapping" };
    return Names;
}

// The same as the menu writes them.
inline const char* GeometryMenuText(EGeometry G)
{
    static const char* const Texts[Geometry_Count] = { "OFF", "POM", "DISPLACEMENT MAPPING" };
    return Texts[(G >= 0 && G < Geometry_Count) ? (int)G : 0];
}

template <typename TChar>
inline void GeometryName(EGeometry G, TChar* Out, int Max)
{
    const char* const N = GeometryNames()[(G >= 0 && G < Geometry_Count) ? (int)G : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Off for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline EGeometry ParseGeometry(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return Geometry_Off;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    for (int i = 0; i < Geometry_Count; i++)
    {
        const char* const N = GeometryNames()[i];
        int k = 0;
        while (N[k] && Text[B + k])
        {
            const TChar a = Text[B + k];
            const char  b = N[k];
            const TChar la = (a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a;
            const char  lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != (TChar)lb)
                break;
            k++;
        }
        if (!N[k] && (!Text[B + k] || Text[B + k] == ' ' || Text[B + k] == '\t' || Text[B + k] == '\r'))
        {
            bKnown = true;
            return (EGeometry)i;
        }
    }
    return Geometry_Off;
}

inline EGeometry GeometryStep(EGeometry G, int Step)
{
    int N = (int)G + Step;
    while (N < 0)
        N += Geometry_Count;
    return (EGeometry)(N % Geometry_Count);
}

// LampShadows: how strongly the lamps around the relief shape it - the share of the baked light their directions
// reshape, the rest being the light the level bounces, which comes from everywhere. Off, or Soft, Medium and Strong:
// 35, 60 and 100 per cent.
enum ELampShadows
{
    LampShadows_Off = 0,
    LampShadows_Soft,
    LampShadows_Medium,
    LampShadows_Strong,
    LampShadows_Count
};

inline const char* const* LampShadowsNames()
{
    static const char* const Names[LampShadows_Count] = { "Off", "Soft", "Medium", "Strong" };
    return Names;
}

inline float LampShadowsStrength(ELampShadows L)
{
    static const float Strength[LampShadows_Count] = { 0.0f, 0.35f, 0.6f, 1.0f };
    return Strength[(L >= 0 && L < LampShadows_Count) ? (int)L : 0];
}

template <typename TChar>
inline void LampShadowsName(ELampShadows L, TChar* Out, int Max)
{
    const char* const N = LampShadowsNames()[(L >= 0 && L < LampShadows_Count) ? (int)L : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Medium for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline ELampShadows ParseLampShadows(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return LampShadows_Medium;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    for (int i = 0; i < LampShadows_Count; i++)
    {
        const char* const N = LampShadowsNames()[i];
        int k = 0;
        while (N[k] && Text[B + k])
        {
            const TChar a = Text[B + k];
            const char  b = N[k];
            const TChar la = (a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a;
            const char  lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != (TChar)lb)
                break;
            k++;
        }
        if (!N[k] && (!Text[B + k] || Text[B + k] == ' ' || Text[B + k] == '\t' || Text[B + k] == '\r'))
        {
            bKnown = true;
            return (ELampShadows)i;
        }
    }
    return LampShadows_Medium;
}

inline ELampShadows LampShadowsStep(ELampShadows L, int Step)
{
    int N = (int)L + Step;
    while (N < 0)
        N += LampShadows_Count;
    return (ELampShadows)(N % LampShadows_Count);
}

// ForceField: how the game's force fields are drawn. Engine: the game's own picture. 3D: the field as a volume of the
// level's space about its surfaces, with arcs, bolts, sparks and a haze in it (FFieldVolume).
enum EForceField
{
    ForceField_Engine = 0,
    ForceField_Volume,
    ForceField_Count
};

inline const char* const* ForceFieldNames()
{
    static const char* const Names[ForceField_Count] = { "Original", "3D" };
    return Names;
}

template <typename TChar>
inline void ForceFieldName(EForceField F, TChar* Out, int Max)
{
    const char* const N = ForceFieldNames()[(F >= 0 && F < ForceField_Count) ? (int)F : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// 3D for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline EForceField ParseForceField(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return ForceField_Volume;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    for (int i = 0; i < ForceField_Count; i++)
    {
        const char* const N = ForceFieldNames()[i];
        int k = 0;
        while (N[k] && Text[B + k])
        {
            const TChar a = Text[B + k];
            const char  b = N[k];
            const TChar la = (a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a;
            const char  lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != (TChar)lb)
                break;
            k++;
        }
        if (!N[k] && (!Text[B + k] || Text[B + k] == ' ' || Text[B + k] == '\t' || Text[B + k] == '\r'))
        {
            bKnown = true;
            return (EForceField)i;
        }
    }
    return ForceField_Volume;
}

inline EForceField ForceFieldStep(EForceField F, int Step)
{
    int N = (int)F + Step;
    while (N < 0)
        N += ForceField_Count;
    return (EForceField)(N % ForceField_Count);
}

// HdtpHoloFix: the game's holograms - characters a level stands somewhere translucent - where the game has put the
// character's HDTP model on them. The game's script gives a hologram its skins by the original models' numbering and
// leaves the black mask as the character's spare skin; an HDTP model that names no skin of its own for its first
// material - its head - is then drawn there in the spare one, which a translucent draw shows nothing of: the hologram
// stands headless, in the game's own renderers too. Off: as the game hands it over. On: the HDTP model with its own
// head. Old: the character's original model, in the skins the script gives that model as a hologram. Both for the
// frame alone - and with either, every hologram of the level, whatever its model, is drawn as the nearest of its
// surfaces alone (FHoloShell).
enum EHoloFix
{
    HoloFix_Off = 0,
    HoloFix_On,
    HoloFix_Old,
    HoloFix_Count
};

inline const char* const* HoloFixNames()
{
    static const char* const Names[HoloFix_Count] = { "Off", "On", "Old" };
    return Names;
}

template <typename TChar>
inline void HoloFixName(EHoloFix F, TChar* Out, int Max)
{
    const char* const N = HoloFixNames()[(F >= 0 && F < HoloFix_Count) ? (int)F : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// On for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline EHoloFix ParseHoloFix(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return HoloFix_On;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    for (int i = 0; i < HoloFix_Count; i++)
    {
        const char* const N = HoloFixNames()[i];
        int k = 0;
        while (N[k] && Text[B + k])
        {
            const TChar a = Text[B + k];
            const char  b = N[k];
            const TChar la = (a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a;
            const char  lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != (TChar)lb)
                break;
            k++;
        }
        if (!N[k] && (!Text[B + k] || Text[B + k] == ' ' || Text[B + k] == '\t' || Text[B + k] == '\r'))
        {
            bKnown = true;
            return (EHoloFix)i;
        }
    }
    return HoloFix_On;
}

inline EHoloFix HoloFixStep(EHoloFix F, int Step)
{
    int N = (int)F + Step;
    while (N < 0)
        N += HoloFix_Count;
    return (EHoloFix)(N % HoloFix_Count);
}

// What the game's script puts on a skin of a character it makes a hologram of (ScriptedPawn.GetStyleTexture), for the
// Old way: no texture at all, the grey mask and the pink mask become the style's own mask - the black one for a
// translucent character (the engine's style 3), the grey for a modulated one (4), the pink for a masked one (2) -,
// the black mask stays the black mask, and any other texture stays what it is.
enum EHoloSkin
{
    HoloSkin_None = 0,
    HoloSkin_Black,
    HoloSkin_Gray,
    HoloSkin_Pink,
    HoloSkin_Other
};

inline EHoloSkin HologramSkin(int Style, EHoloSkin Was)
{
    if (Was == HoloSkin_Other || Was == HoloSkin_Black)
        return Was;
    return (Style == 4) ? HoloSkin_Gray : (Style == 2) ? HoloSkin_Pink : HoloSkin_Black;
}

// On: how many of the game's translucent layers a hologram's nearest surface is laid as (FHoloShell). The game's own
// draw lays every surface the eye's ray meets - of a hull its front and its back: two, and the hologram is then as
// bright as the game makes one. One would be the surface in its own colours alone, fainter than any other hologram.
static const float HoloShellLayers = 2.0f;

// How far from the place a hologram stands on its triangles may be, in the level's units. A see-through triangle in
// a hologram's texture goes to the hologram's pass only from within this: another character of the level may wear
// the same texture, and that one is no hologram. From the cylinder the game gives the character - the larger of its
// two measures, by how much larger than its class the level drew it - and generous: a character's triangles end
// well inside it with its arms out, since one left outside would be drawn as any other see-through mesh.
inline float HologramReach(float Radius, float Height, float Scale)
{
    const float Size = (Radius > Height) ? Radius : Height;
    return 2.5f * Size * ((Scale > 1.0f) ? Scale : 1.0f) + 16.0f;
}

// ---------------------------------------------------------------------------
// The level's fog kept in its rooms (FFogRooms)
// ---------------------------------------------------------------------------

// The engine's fog of one volumetric light at a point is the light's own factor times this (scripts/fog_fit.py fits
// it): the integral of 1 - r^2/R^2 along the ray from the eye, over the part of it inside the light's sphere and before
// the point, divided by R. Dir is the ray's unit direction, Len how far along it the point is. The pass works the same
// sum on the card.
inline float FogTau(const FVec3& Eye, const FVec3& Dir, float Len, const FVec3& Centre, float Radius)
{
    if (!(Radius > 0.0f) || !(Len > 0.0f))
        return 0.0f;
    const FVec3 EC = { Centre.X - Eye.X, Centre.Y - Eye.Y, Centre.Z - Eye.Z };
    const float T0 = Dot(EC, Dir);
    const float H2 = Dot(EC, EC) - T0 * T0;
    const float R2 = Radius * Radius;
    if (H2 >= R2)
        return 0.0f;
    const float W = Sqrt(R2 - H2);
    const float A = (T0 - W > 0.0f) ? (T0 - W) : 0.0f;
    const float B = (T0 + W < Len) ? (T0 + W) : Len;
    if (B <= A)
        return 0.0f;
    const float DA = A - T0, DB = B - T0;
    return ((B - A) * (1.0f - H2 / R2) - (DB * DB * DB - DA * DA * DA) / (3.0f * R2)) / Radius;
}

// A light's factors before the engine's own fog has shown any of it: a guess from its settings, through the small
// lights scripts/fog_fit.py measured (a thick one, VolumeFog 60, and a thin one, 5). The alpha grows faster than
// VolumeFog; the colour is the light's hue at full brightness (the engine's FGetHSV without its brightness curve),
// times VolumeBrightness and the same curve of VolumeFog. A large sphere comes out stronger than the engine's - those
// are seen whole from inside and learned in play.
inline void FogFactorsGuess(int VolumeBrightness, int VolumeFog, int Hue, int Saturation, float Colour[3], float& Alpha)
{
    float H[3];
    if (Hue < 86)
    {
        H[0] = (85 - Hue) / 85.0f; H[1] = Hue / 85.0f; H[2] = 0.0f;
    }
    else if (Hue < 171)
    {
        H[0] = 0.0f; H[1] = (170 - Hue) / 85.0f; H[2] = (Hue - 85) / 85.0f;
    }
    else
    {
        H[0] = (Hue - 170) / 85.0f; H[1] = 0.0f; H[2] = (255 - Hue) / 84.0f;
    }
    const float S = (float)Saturation / 255.0f;
    const float F = (float)VolumeFog;
    const float K = (float)VolumeBrightness * F * (0.0000904f + 6.05e-7f * F);
    for (int i = 0; i < 3; i++)
        Colour[i] = (H[i] + S * (1.0f - H[i])) * K;
    Alpha = F * (0.002312f + 5.76e-5f * F);
}

// Bloom: the halo a bright thing wears. Off, or On: two halos added together - one of everything bright on the
// screen, and a wider one of the engine's coronas, the glow the game itself puts on its lamps (most of which are not
// the level's real lights). The ini key reads the earlier Full and Coronas as On too.
enum EBloom
{
    Bloom_Off = 0,
    Bloom_On,
    Bloom_Count
};

inline const char* const* BloomNames()
{
    static const char* const Names[Bloom_Count] = { "Off", "On" };
    return Names;
}

template <typename TChar>
inline void BloomName(EBloom B, TChar* Out, int Max)
{
    const char* const N = BloomNames()[(B >= 0 && B < Bloom_Count) ? (int)B : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Off for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline EBloom ParseBloom(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return Bloom_Off;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    static const char* const Aliases[2] = { "Full", "Coronas" };
    static const EBloom      AliasIs[2] = { Bloom_On, Bloom_On };
    for (int i = 0; i < Bloom_Count + 2; i++)
    {
        const char* const N = (i < Bloom_Count) ? BloomNames()[i] : Aliases[i - Bloom_Count];
        int k = 0;
        while (N[k] && Text[B + k])
        {
            const TChar a = Text[B + k];
            const char  b = N[k];
            const TChar la = (a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a;
            const char  lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != (TChar)lb)
                break;
            k++;
        }
        if (!N[k] && (!Text[B + k] || Text[B + k] == ' ' || Text[B + k] == '\t' || Text[B + k] == '\r'))
        {
            bKnown = true;
            if (i < Bloom_Count)
                return (EBloom)i;
            return AliasIs[i - Bloom_Count];
        }
    }
    return Bloom_Off;
}

inline EBloom BloomStep(EBloom B, int Step)
{
    int N = (int)B + Step;
    while (N < 0)
        N += Bloom_Count;
    return (EBloom)(N % Bloom_Count);
}

// LensFlare: the ghosts a lens throws of a strong light. Off, or On: of the engine's coronas, which the engine already
// hides when their lamp is out of sight and crops at the screen's edge. The ini key reads the earlier Coronas and
// Picture as On too.
enum ELensFlare
{
    LensFlare_Off = 0,
    LensFlare_On,
    LensFlare_Count
};

inline const char* const* LensFlareNames()
{
    static const char* const Names[LensFlare_Count] = { "Off", "On" };
    return Names;
}

template <typename TChar>
inline void LensFlareName(ELensFlare F, TChar* Out, int Max)
{
    const char* const N = LensFlareNames()[(F >= 0 && F < LensFlare_Count) ? (int)F : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Off for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline ELensFlare ParseLensFlare(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return LensFlare_Off;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    static const char* const Aliases[2] = { "Coronas", "Picture" };
    static const ELensFlare  AliasIs[2] = { LensFlare_On, LensFlare_On };
    for (int i = 0; i < LensFlare_Count + 2; i++)
    {
        const char* const N = (i < LensFlare_Count) ? LensFlareNames()[i] : Aliases[i - LensFlare_Count];
        int k = 0;
        while (N[k] && Text[B + k])
        {
            const TChar a = Text[B + k];
            const char  b = N[k];
            const TChar la = (a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a;
            const char  lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != (TChar)lb)
                break;
            k++;
        }
        if (!N[k] && (!Text[B + k] || Text[B + k] == ' ' || Text[B + k] == '\t' || Text[B + k] == '\r'))
        {
            bKnown = true;
            if (i < LensFlare_Count)
                return (ELensFlare)i;
            return AliasIs[i - LensFlare_Count];
        }
    }
    return LensFlare_Off;
}

inline ELensFlare LensFlareStep(ELensFlare F, int Step)
{
    int N = (int)F + Step;
    while (N < 0)
        N += LensFlare_Count;
    return (ELensFlare)(N % LensFlare_Count);
}

// A side of a smaller set's map, from the texture's side: 1/Shrink of it, but never below 128 texels, and never smaller
// than a side that is 128 or less already. The same rule makes the maps (scripts/height_shrink.py) and finds them
// again: a map is the texture's own only when its size is exactly this.
inline float PomFloor(float X)
{
    const float T = (float)(int)X;
    return (T > X) ? T - 1.0f : T;
}

// Whether a surface's texture is laid on it exactly once - the surface's coordinates all within one whole tile of it,
// give or take a hundredth - and which tile that is (its lower corner). A sign or a poster is mapped so; a wall's
// texture tiles. On one laid on once the relief must not slide past the texture's edge, where the wrap would bring in
// the far side of the picture.
inline bool PomOnceMapped(float MinU, float MaxU, float MinV, float MaxV, float& TileU, float& TileV)
{
    const float Tol = 0.01f;
    TileU = PomFloor(MinU + Tol);
    TileV = PomFloor(MinV + Tol);
    return MaxU > MinU && MaxV > MinV && MaxU <= TileU + 1.0f + Tol && MaxV <= TileV + 1.0f + Tol;
}

inline int PomMapSide(int TextureSide, int Shrink)
{
    const int Floor = 128;
    if (TextureSide <= Floor || Shrink <= 1)
        return TextureSide;
    const int Side = TextureSide / Shrink;
    return Side < Floor ? Floor : Side;
}

// How deep the relief goes, in the level's own units. It is NOT in texels: the same texture sits on the walls at
// whatever scale the level's builder chose, and a depth counted in texels would be twice as deep on a wall that tiles
// it half as often. The shader turns this into a texture-coordinate offset with the surface's own scale.
// Measured on the UNATCO blast door with the sweep in docs/ejszakai-naplo.md: 3 is barely there, 6 reads as
// relief and still looks like a door, 15 pulls the recesses out of shape. Per texture this will be the
// player's own number; this is what a texture starts at.
//
// It starts LOW on purpose. These textures were painted for a game that had no relief at all, and their painted
// shading already says where the grooves are; a depth that would suit a texture authored for this reads as a
// caricature on them. A player who wants one deeper has the menu for it, texture by texture, and that is the way
// round it should be - the whole game milder, the few that earn it deeper.
static const float PomDepthWorld = 2.0f;

// Soft particles: how far in front of what is behind it a translucent surface fades out, in the level's units. A
// player is about 78 units tall, so this is a bit under his height - the scale of the gas clouds themselves, and
// far enough that the straight line where a smoke sprite cuts into the road is gone. Swept in place with the marker
// SecondRevision-soft-fade.txt; 24, 64 and 200 all removed the line, and 64 is the middle one that does not visibly
// thin a cloud standing near a wall. A first value, not a tuned one.
static const float SoftFadeWorld = 64.0f;

// A surface's own axes, with how far along the texture one unit of the level goes. The mapping axes the engine hands
// over are NOT unit vectors - their length carries the texture's scale - so both are wanted: the direction, and the
// length that turns the level's units into texture ones.
struct FPomFrame
{
    FVec3 T, B, N;      // along the texture's U, along its V, and out of the surface
    float Du, Dv;       // texture units per unit of the level, along T and along B
};

inline FPomFrame PomFrameFrom(const FVec3& MapX, const FVec3& MapY, float InvU, float InvV)
{
    FPomFrame F;
    const float LenX = Sqrt(Dot(MapX, MapX));
    const float LenY = Sqrt(Dot(MapY, MapY));
    const float SafeX = (LenX > 1e-6f) ? LenX : 1.0f;
    const float SafeY = (LenY > 1e-6f) ? LenY : 1.0f;
    F.T.X = MapX.X / SafeX; F.T.Y = MapX.Y / SafeX; F.T.Z = MapX.Z / SafeX;
    F.B.X = MapY.X / SafeY; F.B.Y = MapY.Y / SafeY; F.B.Z = MapY.Z / SafeY;
    F.N.X = F.T.Y * F.B.Z - F.T.Z * F.B.Y;
    F.N.Y = F.T.Z * F.B.X - F.T.X * F.B.Z;
    F.N.Z = F.T.X * F.B.Y - F.T.Y * F.B.X;
    const float LenN = Sqrt(Dot(F.N, F.N));
    const float SafeN = (LenN > 1e-6f) ? LenN : 1.0f;
    F.N.X /= SafeN; F.N.Y /= SafeN; F.N.Z /= SafeN;
    F.Du = LenX * InvU;
    F.Dv = LenY * InvV;
    return F;
}

// What one vertex hands the shader: the direction TO THE EYE, said in the surface's own axes, with the texture's
// scale already in the two sideways parts. The engine gives surfaces in the view's own space, so the eye is the
// origin and the direction to it is simply the vertex's place, negated.
//
// A DIRECTION is handed over, not the sliding itself. The sliding is the sideways parts divided by the outward one,
// and that division must happen per PIXEL: a quotient interpolated across a polygon is not the quotient of the
// interpolated parts, and on a big floor the difference is large enough to see - the relief jumps about as the eye
// moves. The direction interpolates properly - AS LONG AS IT IS NOT BROUGHT TO LENGTH ONE at the vertex: the way to
// the eye is the vertex's place negated, which runs evenly across a flat polygon, and its outward part is the eye's
// height over the surface, the same at every corner. Brought to length one, a corner at the player's feet weighs as
// much as one a hundred paces off, what lies between them slides by a fraction of what it should, and by another
// fraction whenever the view cuts the polygon to other corners - a floor's relief then changes in jumps as the
// head tilts. So: the way itself, in the level's units, and the division per pixel.
//
// PD is no distance: the texture's own rate (the mean of its two), the same at every corner, by which the shader
// turns the slide back into the tangent of the angle the surface is seen at - what it holds the slide by, edge on.
//
// The outward part is always handed over positive, so the shader has one case to handle instead of two. Only that
// part: the sideways ones are the texture's own axes and keep their sign whichever way U x V points. In the engine's
// view space Y runs DOWN, so on an upright texture (U to the right, V down) U x V points INTO the wall - turning the
// sideways parts round with it slides the relief towards the eye instead of away, and a boss on a wall then seems to
// turn as the eye goes past it.
inline void PomVertex(const FPomFrame& F, const FVec3& Point, float& PX, float& PY, float& PN, float& PD)
{
    FVec3 V;
    V.X = -Point.X; V.Y = -Point.Y; V.Z = -Point.Z;
    const float Vn   = Dot(V, F.N);
    const float Rate = F.Du * F.Dv;
    PX = Dot(V, F.T) * F.Du;
    PY = Dot(V, F.B) * F.Dv;
    PN = (Vn >= 0.0f) ? Vn : -Vn;
    PD = (Rate > 0.0f) ? Sqrt(Rate) : 1.0f;
}

// The force field as a volume of the level's space (ForceField=3D, FFieldVolume): how far from one of its surfaces the
// field reaches, in the level's units - a body five centimetres thick about each surface.
static const float FieldHalf = 1.3f;
// How bright a field's volume is laid on, by which of the game's fields it is (FieldKind): against one for an arc
// seen face on as bright as the pattern makes it. The cabinets' field is small and stands in lit rooms; the other
// two fill doorways and floors, and are laid on fainter.
inline float FieldBrightOf(int Kind)
{
    return (Kind == 2) ? 0.5f : (Kind == 1 || Kind == 3) ? 0.3f : 0.0f;
}
// A field's hue, held steady. The game repaints a field's texture every frame, and one of the three is a few sparks
// on black: what one frame's sample of the texture says of its hue is then now a spark's, now nothing at all, and a
// field laid on in that hue would come and go with it. So the hue is held from frame to frame. Sum is a sample of
// the texture's colours added up (each from nothing to one): where there is at least a quarter of one bright texel
// of light in it, its hue - brought to full strength - moves the held one a tenth of the way to itself, and the
// first such sample sets it outright; a sample with less light than that leaves the held hue as it is.
inline void FieldHueHold(const float* Sum, float* Held, bool& bKnown)
{
    float Most = Sum[0];
    if (Sum[1] > Most)
        Most = Sum[1];
    if (Sum[2] > Most)
        Most = Sum[2];
    if (!(Most >= 0.25f))
        return;
    for (int i = 0; i < 3; i++)
    {
        const float Now = Sum[i] / Most;
        Held[i] = bKnown ? Held[i] + (Now - Held[i]) * 0.1f : Now;
    }
    bKnown = true;
}

// The most corners a polygon is handed to the pass with, and the rows of four numbers it takes in the pass's list:
// its plane, a ball about it, its hue, its corners (FFieldVolume::CORNERS, ROWS - the same numbers).
enum { FieldCorners = 12, FieldRows = FieldCorners + 3 };

// The pass's noise: a number of no pattern for a place of a cube's grid and one of four channels. The card reads it
// smoothly between the places; the cube goes round at its sides, so the grid's size is the caller's to wrap by.
inline unsigned short FieldNoiseAt(int X, int Y, int Z, int Channel)
{
    unsigned int H = (unsigned int)X * 0x9E3779B1u ^ (unsigned int)Y * 0x85EBCA77u ^ (unsigned int)Z * 0xC2B2AE3Du ^
                     (unsigned int)Channel * 0x27D4EB2Fu;
    H ^= H >> 15;
    H *= 0x2C1B3C6Du;
    H ^= H >> 12;
    H *= 0x297A2D39u;
    H ^= H >> 15;
    return (unsigned short)(H >> 16);
}

// One polygon of the field as the pass takes it: FieldRows rows of four numbers. The plane's normal with the number
// of corners; a ball about the polygon - its middle from the eye, its radius with the half thickness; the look of
// the field it is of - its hue, and how bright it is laid on (Look's four numbers); then the corners from the eye,
// going round so that an edge crossed with the normal points out of the polygon - whichever way they were handed
// over. Pts are the level's own, in the world; convex, in one plane. False for fewer than three corners, more than
// FieldCorners, or no area to speak of: the rows are then not written.
inline bool FieldPolygonRows(const FVec3* Pts, int Count, const FVec3& Eye, float Half, const float* Look, float* Rows)
{
    if (Count < 3 || Count > FieldCorners)
        return false;
    FVec3 N = { 0.0f, 0.0f, 0.0f };
    FVec3 M = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < Count; i++)
    {
        const FVec3& A = Pts[i];
        const FVec3& B = Pts[(i + 1) % Count];
        N.X += (A.Y - B.Y) * (A.Z + B.Z);
        N.Y += (A.Z - B.Z) * (A.X + B.X);
        N.Z += (A.X - B.X) * (A.Y + B.Y);
        M.X += A.X / (float)Count;
        M.Y += A.Y / (float)Count;
        M.Z += A.Z / (float)Count;
    }
    const float Len = Sqrt(Dot(N, N));
    if (!(Len > 1e-3f))
        return false;
    N.X /= Len; N.Y /= Len; N.Z /= Len;
    // Which way round: the first edge crossed with the normal, against the way from the middle to that edge.
    const FVec3 E   = { Pts[1].X - Pts[0].X, Pts[1].Y - Pts[0].Y, Pts[1].Z - Pts[0].Z };
    const FVec3 Out = { E.Y * N.Z - E.Z * N.Y, E.Z * N.X - E.X * N.Z, E.X * N.Y - E.Y * N.X };
    const FVec3 To  = { Pts[0].X + 0.5f * E.X - M.X, Pts[0].Y + 0.5f * E.Y - M.Y, Pts[0].Z + 0.5f * E.Z - M.Z };
    const bool  bTurn = Dot(Out, To) < 0.0f;
    float Far2 = 0.0f;
    for (int i = 0; i < FieldRows * 4; i++)
        Rows[i] = 0.0f;
    for (int i = 0; i < Count; i++)
    {
        const FVec3& A = Pts[bTurn ? Count - 1 - i : i];
        const FVec3  D = { A.X - M.X, A.Y - M.Y, A.Z - M.Z };
        if (Dot(D, D) > Far2)
            Far2 = Dot(D, D);
        Rows[(3 + i) * 4 + 0] = A.X - Eye.X;
        Rows[(3 + i) * 4 + 1] = A.Y - Eye.Y;
        Rows[(3 + i) * 4 + 2] = A.Z - Eye.Z;
    }
    Rows[0] = N.X; Rows[1] = N.Y; Rows[2] = N.Z; Rows[3] = (float)Count;
    Rows[4] = M.X - Eye.X; Rows[5] = M.Y - Eye.Y; Rows[6] = M.Z - Eye.Z; Rows[7] = Sqrt(Far2) + Half;
    Rows[8] = Look[0]; Rows[9] = Look[1]; Rows[10] = Look[2]; Rows[11] = Look[3];
    return true;
}

// Twice a flat polygon's area, signed by which way round its corners go; the corners as pairs of numbers.
inline float FieldTwiceArea(const float* P, int Count)
{
    float Sum = 0.0f;
    for (int i = 0; i < Count; i++)
    {
        const float* const A = P + 2 * i;
        const float* const B = P + 2 * ((i + 1) % Count);
        Sum += A[0] * B[1] - B[0] * A[1];
    }
    return Sum;
}

// Two of the field's polygons as one, where the two together are one convex polygon. The level cuts a surface into
// many (the BSP's cuts through it), a moving brush's faces come cut by the rooms it stands in, and the pass asks
// every polygon it is handed at every place of a ray: so a flat piece of the field goes to it as one polygon, however
// many it was cut into. Both convex, in the world. They are made one only where they lie in one plane and their
// common outline holds nothing that is in neither of them: nothing is added to the field and nothing taken from it.
// The same polygon twice - a field's two faces, back to back - is one. How many corners Out has, FieldCorners at
// most; 0 where the two stay two.
inline int FieldMergedPolygon(const FVec3* A, int NumA, const FVec3* B, int NumB, FVec3* Out)
{
    enum { MOST = 2 * FieldCorners };
    if (NumA < 3 || NumB < 3 || NumA > FieldCorners || NumB > FieldCorners)
        return 0;
    // Both from A's first corner: the numbers stay small, and what is left of them is exact enough for areas.
    const FVec3 O = A[0];
    FVec3 Ra[FieldCorners], Rb[FieldCorners];
    for (int i = 0; i < NumA; i++)
    {
        Ra[i].X = A[i].X - O.X; Ra[i].Y = A[i].Y - O.Y; Ra[i].Z = A[i].Z - O.Z;
    }
    for (int i = 0; i < NumB; i++)
    {
        Rb[i].X = B[i].X - O.X; Rb[i].Y = B[i].Y - O.Y; Rb[i].Z = B[i].Z - O.Z;
    }
    FVec3 N = { 0.0f, 0.0f, 0.0f }, Nb = { 0.0f, 0.0f, 0.0f };
    FVec3 U = { 0.0f, 0.0f, 0.0f };
    float Long2 = 0.0f;
    for (int i = 0; i < NumA; i++)
    {
        const FVec3& P = Ra[i];
        const FVec3& Q = Ra[(i + 1) % NumA];
        N.X += (P.Y - Q.Y) * (P.Z + Q.Z);
        N.Y += (P.Z - Q.Z) * (P.X + Q.X);
        N.Z += (P.X - Q.X) * (P.Y + Q.Y);
        const FVec3 E = { Q.X - P.X, Q.Y - P.Y, Q.Z - P.Z };
        if (Dot(E, E) > Long2)
        {
            Long2 = Dot(E, E);
            U     = E;
        }
    }
    for (int i = 0; i < NumB; i++)
    {
        const FVec3& P = Rb[i];
        const FVec3& Q = Rb[(i + 1) % NumB];
        Nb.X += (P.Y - Q.Y) * (P.Z + Q.Z);
        Nb.Y += (P.Z - Q.Z) * (P.X + Q.X);
        Nb.Z += (P.X - Q.X) * (P.Y + Q.Y);
    }
    const float LenA = Sqrt(Dot(N, N)), LenB = Sqrt(Dot(Nb, Nb));
    if (!(LenA > 1e-3f) || !(LenB > 1e-3f) || !(Long2 > 1e-6f))
        return 0;
    N.X /= LenA; N.Y /= LenA; N.Z /= LenA;
    // One plane: the two face the same way or opposite ways, and none of B's corners is off A's plane.
    const float Same = Dot(N, Nb) / LenB;
    if (Same < 0.9999f && Same > -0.9999f)
        return 0;
    for (int i = 0; i < NumB; i++)
    {
        const float Off = Dot(N, Rb[i]);
        if (Off > 0.05f || Off < -0.05f)
            return 0;
    }
    // Two axes of the plane, and every corner as a pair of numbers along them.
    const float Long = Sqrt(Long2);
    U.X /= Long; U.Y /= Long; U.Z /= Long;
    const FVec3 V = { N.Y * U.Z - N.Z * U.Y, N.Z * U.X - N.X * U.Z, N.X * U.Y - N.Y * U.X };
    float Pa[FieldCorners][2], Pb[FieldCorners][2], All[MOST][2];
    for (int i = 0; i < NumA; i++)
    {
        Pa[i][0] = Dot(Ra[i], U);
        Pa[i][1] = Dot(Ra[i], V);
    }
    for (int i = 0; i < NumB; i++)
    {
        Pb[i][0] = Dot(Rb[i], U);
        Pb[i][1] = Dot(Rb[i], V);
    }
    // Both the same way round: the one that goes the other way is turned.
    if (FieldTwiceArea(&Pa[0][0], NumA) < 0.0f)
        for (int i = 0; i < NumA / 2; i++)
            for (int c = 0; c < 2; c++)
            {
                const float Keep = Pa[i][c];
                Pa[i][c]            = Pa[NumA - 1 - i][c];
                Pa[NumA - 1 - i][c] = Keep;
            }
    if (FieldTwiceArea(&Pb[0][0], NumB) < 0.0f)
        for (int i = 0; i < NumB / 2; i++)
            for (int c = 0; c < 2; c++)
            {
                const float Keep = Pb[i][c];
                Pb[i][c]            = Pb[NumB - 1 - i][c];
                Pb[NumB - 1 - i][c] = Keep;
            }
    const float AreaA = 0.5f * FieldTwiceArea(&Pa[0][0], NumA), AreaB = 0.5f * FieldTwiceArea(&Pb[0][0], NumB);
    const int   Num   = NumA + NumB;
    for (int i = 0; i < NumA; i++)
    {
        All[i][0] = Pa[i][0];
        All[i][1] = Pa[i][1];
    }
    for (int i = 0; i < NumB; i++)
    {
        All[NumA + i][0] = Pb[i][0];
        All[NumA + i][1] = Pb[i][1];
    }
    // Their common outline: the hull of all the corners. In order across the plane first, then round below and
    // back round above; a corner the outline does not turn at is left out.
    for (int i = 1; i < Num; i++)
    {
        const float X = All[i][0], Y = All[i][1];
        int j = i - 1;
        for (; j >= 0 && (All[j][0] > X || (All[j][0] == X && All[j][1] > Y)); j--)
        {
            All[j + 1][0] = All[j][0];
            All[j + 1][1] = All[j][1];
        }
        All[j + 1][0] = X;
        All[j + 1][1] = Y;
    }
    float H[2 * MOST][2];
    int   h = 0;
    for (int Pass = 0; Pass < 2; Pass++)
    {
        const int Floor = h + 1;
        for (int s = 0; s < Num; s++)
        {
            const float* const C = All[Pass ? Num - 1 - s : s];
            while (h >= Floor + 1 &&
                   (H[h - 1][0] - H[h - 2][0]) * (C[1] - H[h - 2][1]) - (H[h - 1][1] - H[h - 2][1]) * (C[0] - H[h - 2][0]) <=
                       0.0f)
                h--;
            H[h][0] = C[0];
            H[h][1] = C[1];
            h++;
        }
        h--;        // the pass's last corner is the next one's first
    }
    // A corner the outline all but goes straight through - a cut's end on an edge, a hair off the line - goes too.
    for (bool bGone = true; bGone && h > 3;)
    {
        bGone = false;
        for (int k = 0; k < h && h > 3; k++)
        {
            const float* const Pv = H[(k + h - 1) % h];
            const float* const Nx = H[(k + 1) % h];
            const float Ex = Nx[0] - Pv[0], Ey = Nx[1] - Pv[1];
            const float Span = Sqrt(Ex * Ex + Ey * Ey);
            const float Lean = Ex * (H[k][1] - Pv[1]) - Ey * (H[k][0] - Pv[0]);
            if (Span > 1e-6f && Lean < 0.02f * Span && Lean > -0.02f * Span)
            {
                for (int m = k; m + 1 < h; m++)
                {
                    H[m][0] = H[m + 1][0];
                    H[m][1] = H[m + 1][1];
                }
                h--;
                bGone = true;
                k--;
            }
        }
    }
    if (h < 3 || h > FieldCorners)
        return 0;
    // What the two share: A cut down to what of it is within B, an edge of B at a time.
    float Cut[MOST + 4][2], Next[MOST + 4][2];
    int   c = NumA;
    for (int i = 0; i < NumA; i++)
    {
        Cut[i][0] = Pa[i][0];
        Cut[i][1] = Pa[i][1];
    }
    for (int e = 0; e < NumB && c >= 3; e++)
    {
        const float* const E0 = Pb[e];
        const float* const E1 = Pb[(e + 1) % NumB];
        const float Ex = E1[0] - E0[0], Ey = E1[1] - E0[1];
        int n = 0;
        for (int k = 0; k < c && n < MOST + 2; k++)
        {
            const float* const P = Cut[k];
            const float* const Q = Cut[(k + 1) % c];
            const float Sp = Ex * (P[1] - E0[1]) - Ey * (P[0] - E0[0]);
            const float Sq = Ex * (Q[1] - E0[1]) - Ey * (Q[0] - E0[0]);
            if (Sp >= 0.0f)
            {
                Next[n][0] = P[0];
                Next[n][1] = P[1];
                n++;
            }
            if ((Sp >= 0.0f) != (Sq >= 0.0f))
            {
                const float R = Sp / (Sp - Sq);
                Next[n][0] = P[0] + (Q[0] - P[0]) * R;
                Next[n][1] = P[1] + (Q[1] - P[1]) * R;
                n++;
            }
        }
        c = n;
        for (int k = 0; k < c; k++)
        {
            Cut[k][0] = Next[k][0];
            Cut[k][1] = Next[k][1];
        }
    }
    const float Shared = (c >= 3) ? 0.5f * FieldTwiceArea(&Cut[0][0], c) : 0.0f;
    // One polygon where the outline holds no more than the two do together - but for a sliver a fiftieth of a unit
    // wide along it, which the corners' own inexactness can make.
    float Round = 0.0f;
    for (int k = 0; k < h; k++)
    {
        const float Ex = H[(k + 1) % h][0] - H[k][0], Ey = H[(k + 1) % h][1] - H[k][1];
        Round += Sqrt(Ex * Ex + Ey * Ey);
    }
    if (0.5f * FieldTwiceArea(&H[0][0], h) - (AreaA + AreaB - Shared) > 0.02f * Round)
        return 0;
    for (int k = 0; k < h; k++)
    {
        Out[k].X = O.X + U.X * H[k][0] + V.X * H[k][1];
        Out[k].Y = O.Y + U.Y * H[k][0] + V.Y * H[k][1];
        Out[k].Z = O.Z + U.Z * H[k][0] + V.Z * H[k][1];
    }
    return h;
}

// A frame's polygons of the field made as few as they will go (FieldMergedPolygon): each is held against every other
// of its kind, and one that has grown against all of them again, until no two will go together. Points holds
// FieldCorners corners' room a polygon, Sizes how many corners each has, Kinds which field each is of - only two of
// one kind are made one. The lists are packed down; how many polygons are left.
inline int FieldMergeAll(FVec3* Points, int* Sizes, int* Kinds, int Count)
{
    for (bool bAgain = true; bAgain;)
    {
        bAgain = false;
        for (int i = 0; i < Count; i++)
            for (int j = i + 1; j < Count;)
            {
                FVec3     One[FieldCorners];
                const int Num = (Kinds[i] == Kinds[j])
                                    ? FieldMergedPolygon(Points + i * FieldCorners, Sizes[i], Points + j * FieldCorners,
                                                         Sizes[j], One)
                                    : 0;
                if (Num < 3)
                {
                    j++;
                    continue;
                }
                for (int k = 0; k < Num; k++)
                    Points[i * FieldCorners + k] = One[k];
                Sizes[i] = Num;
                // The list's last polygon into the place of the one that went.
                Count--;
                if (j < Count)
                {
                    for (int k = 0; k < Sizes[Count]; k++)
                        Points[j * FieldCorners + k] = Points[Count * FieldCorners + k];
                    Sizes[j] = Sizes[Count];
                    Kinds[j] = Kinds[Count];
                }
                bAgain = true;
                j      = i + 1;
            }
    }
    return Count;
}

// Geometry=Tessellation: which edges of a surface's polygons it shares with another polygon of the SAME surface. The
// engine hands a surface over as the level's convex polygons (the BSP's cuts through it), each clipped to the screen.
// Where two of them meet, the relief is lifted from both sides by the same height at the same place, so that edge may
// stand out with the rest; every other edge is where the surface meets a different one - a corner, the rim of a
// doorway - or where the engine cut it at the screen's edge, and there the relief has to come down to the wall.
//
// Shared means: another polygon has an edge lying along this one, running the other way (two neighbours on the same
// side of a plane go round a common edge in opposite directions), and such edges cover at least nine tenths of it -
// more than one may, where the neighbour has a corner in the middle of this edge (a T-junction). Tol is how far off
// the line a point may be and still lie on it, in the level's units.
//
// Pts holds all the polygons' points one after another; polygon p has Counts[p] of them from Starts[p]. Shared gets
// one answer per edge, at the index of the edge's first point.
inline void ReliefSharedEdges(const FVec3* Pts, const int* Starts, const int* Counts, int NumPolys, float Tol,
                              bool* Shared)
{
    // A box around each polygon, widened by Tol: an edge is compared only with the polygons whose box it reaches -
    // its neighbours, not the whole surface. Past the room for boxes every polygon is compared, as without them.
    enum { BoxRoom = 256 };
    float Box[BoxRoom][6];
    const bool bBoxes = NumPolys <= BoxRoom;
    for (int q = 0; bBoxes && q < NumPolys; q++)
    {
        float* const Q = Box[q];
        Q[0] = Q[1] = Q[2] = 1e30f;
        Q[3] = Q[4] = Q[5] = -1e30f;
        for (int i = 0; i < Counts[q]; i++)
        {
            const FVec3& P = Pts[Starts[q] + i];
            Q[0] = (P.X < Q[0]) ? P.X : Q[0];  Q[3] = (P.X > Q[3]) ? P.X : Q[3];
            Q[1] = (P.Y < Q[1]) ? P.Y : Q[1];  Q[4] = (P.Y > Q[4]) ? P.Y : Q[4];
            Q[2] = (P.Z < Q[2]) ? P.Z : Q[2];  Q[5] = (P.Z > Q[5]) ? P.Z : Q[5];
        }
        Q[0] -= Tol; Q[1] -= Tol; Q[2] -= Tol;
        Q[3] += Tol; Q[4] += Tol; Q[5] += Tol;
    }
    for (int p = 0; p < NumPolys; p++)
    {
        const int S = Starts[p], N = Counts[p];
        for (int e = 0; e < N; e++)
        {
            const FVec3& A = Pts[S + e];
            const FVec3& B = Pts[S + (e + 1) % N];
            const FVec3  D = { B.X - A.X, B.Y - A.Y, B.Z - A.Z };
            const float  L2 = Dot(D, D);
            if (!(L2 > 1e-4f))
            {
                Shared[S + e] = true;       // no length (a hundredth of a unit): nothing to hold or lift along it
                continue;
            }
            // The edge's own box.
            const float EdgeLo[3] = { (A.X < B.X) ? A.X : B.X, (A.Y < B.Y) ? A.Y : B.Y, (A.Z < B.Z) ? A.Z : B.Z };
            const float EdgeHi[3] = { (A.X > B.X) ? A.X : B.X, (A.Y > B.Y) ? A.Y : B.Y, (A.Z > B.Z) ? A.Z : B.Z };
            float Covered = 0.0f;           // how much of the edge others lie along, in its own length
            for (int q = 0; q < NumPolys && Covered < 0.9f; q++)
            {
                if (q == p)
                    continue;
                if (bBoxes && (EdgeHi[0] < Box[q][0] || EdgeLo[0] > Box[q][3] || EdgeHi[1] < Box[q][1] || EdgeLo[1] > Box[q][4] ||
                               EdgeHi[2] < Box[q][2] || EdgeLo[2] > Box[q][5]))
                    continue;
                const int T = Starts[q], M = Counts[q];
                for (int f = 0; f < M; f++)
                {
                    const FVec3& C = Pts[T + f];
                    const FVec3& E = Pts[T + (f + 1) % M];
                    const FVec3  AC = { C.X - A.X, C.Y - A.Y, C.Z - A.Z };
                    const FVec3  AE = { E.X - A.X, E.Y - A.Y, E.Z - A.Z };
                    const float  Tc = Dot(AC, D) / L2;      // where C and E fall along A..B, 0 at A and 1 at B
                    const float  Te = Dot(AE, D) / L2;
                    if (!(Te < Tc))
                        continue;                            // not running the other way
                    // Off the line: the part of each that is not along the edge.
                    const FVec3 OffC = { AC.X - D.X * Tc, AC.Y - D.Y * Tc, AC.Z - D.Z * Tc };
                    const FVec3 OffE = { AE.X - D.X * Te, AE.Y - D.Y * Te, AE.Z - D.Z * Te };
                    if (Dot(OffC, OffC) > Tol * Tol || Dot(OffE, OffE) > Tol * Tol)
                        continue;
                    const float Lo = (Te > 0.0f) ? Te : 0.0f;
                    const float Hi = (Tc < 1.0f) ? Tc : 1.0f;
                    if (Hi > Lo)
                        Covered += Hi - Lo;
                }
            }
            Shared[S + e] = Covered >= 0.9f;
        }
    }
}

// Geometry=Tessellation: how far a point is from the segment A..B - the relief comes down to the wall over a band
// along its held edges, and a vertex carries its distance to the nearest of them.
inline float ReliefSegmentDistance(const FVec3& P, const FVec3& A, const FVec3& B)
{
    const FVec3 AB = { B.X - A.X, B.Y - A.Y, B.Z - A.Z };
    const FVec3 AP = { P.X - A.X, P.Y - A.Y, P.Z - A.Z };
    const float L2 = Dot(AB, AB);
    float T = (L2 > 0.0f) ? Dot(AP, AB) / L2 : 0.0f;
    T = (T < 0.0f) ? 0.0f : ((T > 1.0f) ? 1.0f : T);
    const FVec3 Off = { AP.X - AB.X * T, AP.Y - AB.Y * T, AP.Z - AB.Z * T };
    return Sqrt(Dot(Off, Off));
}

// Geometry=Tessellation: the rim at a held edge, by the wall found standing beyond it. Across the edge, in the plane
// through the edge square to it, the wall rises from the edge at an angle to the surface: UpIn is the cosine of that
// angle (the wall's upward direction along the surface's inward one) and UpOut the sine (along the surface's normal).
// Where it stands at the edge and rises no flatter than the band itself would (the band's slope: one depth up over
// Bevel depths across), the relief goes up to it: the rim runs down along the wall, where the wall hides it, rather
// than over a band - Width 0, and Cot, at the edge, how far the relief's top moves inwards per unit it is lifted, so
// that it lies on a wall leaning over the surface (positive) or away from it (negative, over the wall's foot); a wall
// leaning further over than the band's slope is taken at that slope, as the surface under it cannot be seen anyway.
// GapAtFoot is how far beyond the edge the wall's foot stands: as a gap opens - a door leaving its frame - the band
// comes back, all of it at FullGap, and the relief's top leaves the wall with it. Anything else is Width 1, Cot 0.
inline void ReliefRimFromWall(float UpIn, float UpOut, float GapAtFoot, float Bevel, float FullGap, float& Width,
                              float& Cot)
{
    Width = 1.0f;
    Cot   = 0.0f;
    if (!(UpOut > 1e-3f) || !(Bevel > 0.0f) || !(FullGap > 0.0f))
        return;
    const float C = UpIn / UpOut;
    if (C < -Bevel)
        return;                             // flatter than the band: the band's own slope is the steeper one
    const float Gap = (GapAtFoot > 0.0f) ? GapAtFoot : 0.0f;
    if (Gap >= FullGap)
        return;
    Width = Gap / FullGap;
    Cot   = ((C > Bevel) ? Bevel : C) * (1.0f - Width);
}

// Geometry=Tessellation: a convex polygon cut down to the side of a plane where N.P + D >= 0, in place. Made[i] says
// whether the edge from point i to point i + 1 was made by a cut - by this one or an earlier one - rather than being
// (a piece of) one of the polygon's own edges. The relief is lifted along a cut edge like anywhere else, because what
// lies beyond it is only out of view, not a different surface. Max is the room in both arrays; a convex polygon gains at
// most one point a cut. Returns the number of points left - fewer than three and nothing is left of it.
inline int ReliefClipByPlane(FVec3* Pts, bool* Made, int Num, int Max, const FVec3& N, float D)
{
    FVec3     OutPts[64];
    bool      OutMade[64];
    int       Out = 0;
    const int Cap = (Max < 64) ? Max : 64;
    if (Num < 3 || Num > Cap)
        return 0;
    for (int i = 0; i < Num; i++)
    {
        const FVec3& A  = Pts[i];
        const FVec3& B  = Pts[(i + 1) % Num];
        const float  Da = Dot(N, A) + D;
        const float  Db = Dot(N, B) + D;
        const bool   Ai = Da >= 0.0f, Bi = Db >= 0.0f;
        if (Out + 2 > Cap)
            return 0;                       // no room: better nothing than a wrong shape
        if (Ai)
        {
            OutPts[Out]  = A;
            OutMade[Out] = Made[i];         // its edge is (a piece of) the one it had
            Out++;
        }
        if (Ai != Bi)
        {
            // Always from the inside point towards the outside one, so a neighbour cutting the same edge the other
            // way round finds the very same point.
            const FVec3& In   = Ai ? A : B;
            const FVec3& Away = Ai ? B : A;
            const float  Din  = Ai ? Da : Db;
            const float  Dout = Ai ? Db : Da;
            const float  T    = Din / (Din - Dout);
            FVec3 P;
            P.X = In.X + (Away.X - In.X) * T;
            P.Y = In.Y + (Away.Y - In.Y) * T;
            P.Z = In.Z + (Away.Z - In.Z) * T;
            OutPts[Out]  = P;
            // Leaving, the edge from here runs along the cut; coming back in, the rest of this edge is still its own.
            OutMade[Out] = Ai ? true : Made[i];
            Out++;
        }
    }
    for (int i = 0; i < Out; i++)
    {
        Pts[i]  = OutPts[i];
        Made[i] = OutMade[i];
    }
    return Out >= 3 ? Out : 0;
}

// Geometry=Tessellation: the view's five planes in view space, inside where N.P + D >= 0 - the frame's four edges on
// the screen (the engine puts a point at X * Focal / Z + CentreX, the frame spans 0..Width, and so down the screen),
// each moved out by Margin units of the level, and the near plane a unit from the eye, where the GPU clips anyway. A
// polygon cut to these still holds everything that, lifted by at most Margin, could come into view.
inline void ReliefViewPlanes(float Focal, float CentreX, float CentreY, float Width, float Height, float Margin,
                             FVec3 N[5], float D[5])
{
    const FVec3 Raw[4] = { { Focal, 0.0f, CentreX }, { -Focal, 0.0f, Width - CentreX },
                           { 0.0f, Focal, CentreY }, { 0.0f, -Focal, Height - CentreY } };
    for (int i = 0; i < 4; i++)
    {
        const float Len = Sqrt(Dot(Raw[i], Raw[i]));
        N[i].X = Raw[i].X / Len;
        N[i].Y = Raw[i].Y / Len;
        N[i].Z = Raw[i].Z / Len;
        D[i]   = Margin;
    }
    N[4].X = 0.0f;
    N[4].Y = 0.0f;
    N[4].Z = 1.0f;
    D[4]   = -1.0f;
}

// The surface's normal in view space, turned towards the eye (the origin), from all its polygons at once: each one's
// Newell sum, which does not care whether the polygon has a corner that lies on a straight line. False when the
// polygons have no area to speak of - then there is no plane to lift anything out of.
inline bool ReliefNormalTowardsEye(const FVec3* Pts, const int* Starts, const int* Counts, int NumPolys, FVec3& Out)
{
    FVec3 N = { 0.0f, 0.0f, 0.0f };
    for (int p = 0; p < NumPolys; p++)
    {
        const int S = Starts[p], Cn = Counts[p];
        for (int i = 0; i < Cn; i++)
        {
            const FVec3& A = Pts[S + i];
            const FVec3& B = Pts[S + (i + 1) % Cn];
            N.X += (A.Y - B.Y) * (A.Z + B.Z);
            N.Y += (A.Z - B.Z) * (A.X + B.X);
            N.Z += (A.X - B.X) * (A.Y + B.Y);
        }
    }
    const float Len = Sqrt(Dot(N, N));
    if (!(Len > 1e-3f) || NumPolys < 1 || Counts[0] < 1)
        return false;
    N.X /= Len; N.Y /= Len; N.Z /= Len;
    // Towards the eye: from a point of the surface, the eye lies along -Point, so the normal must make a positive
    // product with it.
    if (Dot(N, Pts[Starts[0]]) > 0.0f)
    {
        N.X = -N.X; N.Y = -N.Y; N.Z = -N.Z;
    }
    Out = N;
    return true;
}

// The frame's raised planes, for the decals that lie on them (the render device's ReliefDecalLift). A wall is many
// surfaces in one plane, and a frame raises a thousand surfaces: a plane is kept once. Two surfaces are in one plane
// where their normals and their distances agree this closely - view space, N out of the surface, D its N . P.
const float ReliefPlaneSameN = 1.0e-3f;
const float ReliefPlaneSameD = 0.05f;
inline bool ReliefSamePlane(const FVec3& NA, float DA, const FVec3& NB, float DB)
{
    const float dx = NA.X - NB.X, dy = NA.Y - NB.Y, dz = NA.Z - NB.Z, dd = DA - DB;
    return dx < ReliefPlaneSameN && dx > -ReliefPlaneSameN && dy < ReliefPlaneSameN && dy > -ReliefPlaneSameN &&
           dz < ReliefPlaneSameN && dz > -ReliefPlaneSameN && dd < ReliefPlaneSameD && dd > -ReliefPlaneSameD;
}
// Where a plane is looked for among the frame's: its normal in steps of a thirty-second, its distance in whole units.
// One plane found twice a hair apart is looked for in the same place unless the two lie either side of a step - then
// it is kept twice, which costs a place in the list and changes nothing a decal sees.
inline unsigned int ReliefPlaneKey(const FVec3& N, float D)
{
    const unsigned int X   = (unsigned int)(int)((N.X + 2.0f) * 32.0f);
    const unsigned int Y   = (unsigned int)(int)((N.Y + 2.0f) * 32.0f);
    const unsigned int Z   = (unsigned int)(int)((N.Z + 2.0f) * 32.0f);
    const float        Far = (D < -1.0e6f) ? -1.0e6f : ((D > 1.0e6f) ? 1.0e6f : D);
    const unsigned int W   = (unsigned int)(int)(Far + 1.0e6f);
    return (X * 73856093u) ^ (Y * 19349663u) ^ (Z * 83492791u) ^ (W * 2654435761u);
}

// Geometry=Tessellation: how far the relief may be lifted at a point before it reaches something standing above it - a
// poster, a mirror or a painting on a wall, a rug on a floor - so that it never pokes through what hangs or lies on it.
// DSRelief does the same, step for step (ReliefClear), on the depth of everything else the world draws, flat, taken just
// before the relief is drawn (FSolidPass, the covers). The numbers are the shader's RELIEF_CLEAR_*.
//
// P is the point on the flat surface and D the direction it is lifted along, in view space (the eye at the origin); N is
// the surface's normal towards the eye, and Lift how far the point would go. CoverZ(Q) is the view depth of the nearest
// cover on the eye's ray through Q - a huge number where there is none. PixelAngle is what one pixel of the covers' depth
// spans at a unit's distance: the depth is read at a pixel's middle, not on the ray itself, so a surface read there seems
// up to half a pixel's width off its place - its own surface too, just above itself where the view grazes it.
//
// Walked up from the surface a step at a time. What the eye sees on the ray through the lifted point, if it lies above
// the surface by more than a pixel's width and within a step of the point's own height, may stand over the point - and
// it does if what the eye sees on the ray through the point's own line at that height is at that height too, give or
// take a pixel's width. A cover lying along the surface is at the same height wherever the eye meets it; the surface
// itself, a wall beside it, or anything between the eye and it, is not - so none of them counts, however slanted the
// view, bar a wall within a pixel or so of the point.
enum { ReliefClearSteps = 8 };
const float ReliefClearMin = 0.02f;     // the first step's height: anything lower is the surface itself
const float ReliefClearEps = 0.02f;     // how far from the height found a cover may be read, besides a pixel's width
const float ReliefClearGap = 0.05f;     // how far below a cover the relief stops, besides a pixel's width

template <typename TCoverZ>
inline float ReliefClearance(const FVec3& P, const FVec3& D, const FVec3& N, float Lift, float PixelAngle,
                             TCoverZ CoverZ)
{
    if (!(Lift > 0.0f))
        return Lift;
    float Up = Dot(D, N);
    Up = (Up > 0.25f) ? Up : 0.25f;
    const float Step = Lift / (float)ReliefClearSteps;
    for (int k = 0; k < ReliefClearSteps; k++)
    {
        const float G = ReliefClearMin + (float)k * Step;
        const FVec3 Q = { P.X + D.X * G, P.Y + D.Y * G, P.Z + D.Z * G };
        if (!(Q.Z >= 1.0f))
            break;
        // What the eye sees on Q's ray, and how far along D above the surface it is.
        const float R  = CoverZ(Q) / Q.Z;
        const FVec3 SP = { Q.X * R - P.X, Q.Y * R - P.Y, Q.Z * R - P.Z };
        const float H  = Dot(SP, N) / Up;
        const float Near = (H > G) ? (H - G) : (G - H);
        if (!(H > ReliefClearMin + Q.Z * PixelAngle) || !(Near <= Step))
            continue;
        // On P's own line at that height, is what the eye sees there at that height too?
        const FVec3 C  = { P.X + D.X * H, P.Y + D.Y * H, P.Z + D.Z * H };
        const float RC = CoverZ(C) / C.Z;
        const FVec3 CP = { C.X * RC - P.X, C.Y * RC - P.Y, C.Z * RC - P.Z };
        const float HC = Dot(CP, N) / Up;
        const float Off = (HC > H) ? (HC - H) : (H - HC);
        if (Off <= ReliefClearEps + C.Z * PixelAngle)
        {
            // Under it by the gap, and by a pixel's width there besides: the cover's height is known no better.
            const float Clear = H - ReliefClearGap - C.Z * PixelAngle;
            return (Clear < Lift) ? ((Clear > 0.0f) ? Clear : 0.0f) : Lift;
        }
    }
    // And the lifted point itself: what the eye sees on its own ray, where the point stands in front of it and it
    // lies along the surface at a height the point has passed, is a cover the steps went by.
    const FVec3 T = { P.X + D.X * Lift, P.Y + D.Y * Lift, P.Z + D.Z * Lift };
    if (T.Z >= 1.0f)
    {
        const float ZT = CoverZ(T);
        const float RT = ZT / T.Z;
        const FVec3 ST = { T.X * RT - P.X, T.Y * RT - P.Y, T.Z * RT - P.Z };
        const float HT = Dot(ST, N) / Up;
        if (ZT > T.Z && HT > ReliefClearMin + T.Z * PixelAngle && HT < Lift)
        {
            const FVec3 C  = { P.X + D.X * HT, P.Y + D.Y * HT, P.Z + D.Z * HT };
            const float RC = CoverZ(C) / C.Z;
            const FVec3 CP = { C.X * RC - P.X, C.Y * RC - P.Y, C.Z * RC - P.Z };
            const float HC = Dot(CP, N) / Up;
            const float Off = (HC > HT) ? (HC - HT) : (HT - HC);
            if (Off <= ReliefClearEps + C.Z * PixelAngle)
            {
                const float Clear = HT - ReliefClearGap - C.Z * PixelAngle;
                return (Clear > 0.0f) ? Clear : 0.0f;
            }
        }
    }
    return Lift;
}

// One line of the relief list (SecondRevision.pom\textures.txt): a texture's name, on or off, and how deep - the depth
// may be left out. A name with a space in it - a few of the game's textures have one (CoreTexCeramic.Clay ReddRoofT) -
// stands in double quotes; any other name may too. Anything after a # is a note. False for a line that says none of
// this: blank, only a note, a name with no on or off after it, or a quote that is not closed. The key comes out lower
// case, without the quotes.
inline bool PomListWordEnds(char C)
{
    return C == 0 || C == ' ' || C == '\t' || C == '#' || C == '\r' || C == '\n';
}

inline char PomListLower(char C)
{
    return (C >= 'A' && C <= 'Z') ? (char)(C + 32) : C;
}

inline bool ParsePomListLine(const char* Line, char* Key, int KeyMax, bool& bOn, float& Depth)
{
    Depth = PomDepthWorld;
    bOn   = false;
    if (!Line || !Key || KeyMax < 2)
        return false;
    Key[0] = 0;
    int p = 0;
    while (Line[p] == ' ' || Line[p] == '\t')
        p++;
    int k = 0;
    if (Line[p] == '"')
    {
        p++;
        while (Line[p] && Line[p] != '"' && Line[p] != '\r' && Line[p] != '\n')
        {
            if (k < KeyMax - 1)
                Key[k++] = PomListLower(Line[p]);
            p++;
        }
        if (Line[p] != '"')
        {
            Key[0] = 0;
            return false;
        }
        p++;
    }
    else
        while (!PomListWordEnds(Line[p]))
        {
            if (k < KeyMax - 1)
                Key[k++] = PomListLower(Line[p]);
            p++;
        }
    Key[k] = 0;
    if (k == 0)
        return false;
    while (Line[p] == ' ' || Line[p] == '\t')
        p++;
    // Each character is looked at only once the one before it has matched, so nothing is read past the line's end.
    if (PomListLower(Line[p]) == 'o' && PomListLower(Line[p + 1]) == 'n' && PomListWordEnds(Line[p + 2]))
    {
        bOn = true;
        p += 2;
    }
    else if (PomListLower(Line[p]) == 'o' && PomListLower(Line[p + 1]) == 'f' && PomListLower(Line[p + 2]) == 'f' &&
             PomListWordEnds(Line[p + 3]))
        p += 3;
    else
    {
        Key[0] = 0;
        return false;
    }
    while (Line[p] == ' ' || Line[p] == '\t')
        p++;
    if (Line[p] >= '0' && Line[p] <= '9')
    {
        float Whole = 0.0f;
        while (Line[p] >= '0' && Line[p] <= '9')
            Whole = Whole * 10.0f + (float)(Line[p++] - '0');
        if (Line[p] == '.' || Line[p] == ',')
        {
            p++;
            float Scale = 0.1f;
            while (Line[p] >= '0' && Line[p] <= '9')
            {
                Whole += (float)(Line[p++] - '0') * Scale;
                Scale *= 0.1f;
            }
        }
        Depth = Whole;
    }
    return true;
}

// What the shader is told about a run's relief, packed into four numbers: how deep it goes in the level's units, what
// is switched on, and one over the height map's size.
// Whether a point on the screen falls inside a polygon given as a fan of points. Used to tell which surface the
// crosshair is on, which is what lets a player say yes or no to the relief of the very texture in front of them.
//
// The points come round the polygon in order and the shape is convex, which is true of every one the engine hands
// over: the point is inside when it is on the same side of every edge. A polygon wound the other way is handled by
// accepting either side, as long as it is the SAME side all the way round.
inline bool PointInFan(const float* X, const float* Y, int Count, float Px, float Py)
{
    if (Count < 3)
        return false;
    int Positive = 0, Negative = 0;
    for (int i = 0; i < Count; i++)
    {
        const int j = (i + 1 < Count) ? (i + 1) : 0;
        const float Cross = (X[j] - X[i]) * (Py - Y[i]) - (Y[j] - Y[i]) * (Px - X[i]);
        if (Cross > 0.0f)
            Positive++;
        else if (Cross < 0.0f)
            Negative++;
        if (Positive && Negative)
            return false;
    }
    return true;
}

// The four spare numbers in the run's constants carry the relief on an opaque surface and the soft fade on a
// translucent one, so the flags below share one word - no surface is ever both.
enum EPomFlagBits
{
    PomFlag_On         = 1,   // there is a height map for this surface
    // Not the relief at all: this translucent run fades out where it meets what is behind it (soft particles).
    PomFlag_Soft       = 8,
    // The texture is laid on this surface exactly once (RdxMath::PomOnceMapped): the relief stays inside it.
    PomFlag_OnceMapped = 32,
    // Geometry=Tessellation: the surface itself is lifted by the height map (DSRelief) - the pixel shader walks nothing.
    PomFlag_Tess       = 64,
    // LampShadows: the relief lit and shadowed by the lamps around it (the shader's ReliefLamps).
    PomFlag_Lamps      = 128,
    // A measurement view, marker only: the tessellated relief tinted where it is held down towards an edge.
    PomFlag_ShowKeep   = 256
};

// LampShadows: the most of the shadows' lights the relief is lit by in a frame - the shader's rows (RDX_RELIEF_LAMP_MAX).
// The relief takes the shadows' own pick, in its order, so the light it is shaded from is the light the scene's shadows
// are cast by.
enum { ReliefLampMax = RDX_RELIEF_LAMP_MAX };

// Smoke, gas and steam are flat pictures standing in the air, and the game's own artists put every one of them in a
// single GROUP: Effects.Smoke. That group IS the list - there is no hand kept table of names here, and nothing to
// keep up to date when a texture pack replaces the pictures, because a pack replaces the picture and not the name.
//
// Read out of the packages (scripts/content_query.py textures effects.smoke): the group holds exactly seven, and no
// other texture in the game is in it. The others the word "smoke" turns up - a no-smoking sign, a billboard, some
// palettes - are in other groups, which is the whole reason to match on the group and not on the name.
//
//     Effects.Smoke.Gas_Poison      Effects.Smoke.SmokePuff
//     Effects.Smoke.Gas_Poison_A    Effects.Smoke.SmokePuff1
//     Effects.Smoke.Gas_Tear        Effects.Smoke.SmokeSpray1
//     Effects.Smoke.Gas_Tear_A
//
// The group is not the whole rule, though: what WEARS these is not always smoke. Revision's dust and snow flakes wear
// Gas_Tear, lit by the level - so a card is smoke only where it is drawn as the game draws its smoke, see-through and
// with a brightness of its own (the draw path's bCloudCard). And smoke comes in every size: the game's own generators
// make cards from a unit and a half across (where a bullet struck) to ninety (the steam of a vent), an incense
// burner's a dozen.
//
// How a cloud is shaped and lit. First values, not tuned ones - swept in play from the menu is what these are for.
//   Grain    repeats of the noise per unit of the level. Shaped on puffs 30-80 units across, where it gives a handful
//            of lumps rather than either a smooth ball or a grey fog of static; smaller smoke is carved finer
//            (CloudGrainSize).
//   Depth    how thick ONE PUFF is, through its middle (CloudPuffDepth below): what the walk's cover comes from,
//            1 - exp(-density * length), the density a puff's own.
//   Steps    how many samples along the ray. The quad bounds the pixels; this bounds the work inside them.
//   Ambient  what the cloud is lit by where no lamp of ours reaches it. Without this a cloud in an unlit corridor
//            would be black, and black smoke over a dark floor is nothing at all.
static const float CloudGrainWorld = 0.075f;
static const float CloudSteps      = 24.0f;   // the CEILING on the count now, not the count itself
//   StepWorld the length one step aims for, the level's units. The count follows from it and the stretch
//            actually walked: one puff on its own is a short stretch and wants few steps - cheaper than a
//            fixed count - while two clouds at opposite ends of a corridor are a long one and want more, or
//            each of them gets three samples and turns to porridge.
static const float CloudStepWorld  = 11.0f;
static const float CloudAmbient    = 0.60f;
//   Rise     units a second the smoke climbs THROUGH the noise. The pattern stands still in the world and the
//            cloud moves up through it, which is what the eye reads as rising - a pattern that merely slides with
//            the cloud reads as a moving stain instead. Steam out of a vent goes up fast; this is that speed.
//   Warp     how far a second, much slower noise bends WHERE the first one is read. Nothing else here is the
//            difference between smoke that curls and a pattern that slides across: without it every wisp keeps its
//            shape for ever and only travels.
//   SelfShade how far towards the lamp a step looks for smoke standing in its way, the level's units. THIS is what
//            makes a volume read as a volume: without it nothing inside a cloud is darker than its edge, and an
//            even grey is exactly what the flat card already was.
// How fast the smoke climbs through the noise. The game's own sprite rises a good deal faster than this; Laci's
// call is to stay slower than the original on purpose, but not by as much as 26 was.
static const float CloudRiseWorld  = 48.0f;
static const float CloudWarpWorld  = 1.6f;
static const float CloudSelfShade  = 45.0f;
//   Fatness  how much of the card the ball actually fills. A sprite is a square of cloth with a round cloud painted
//            on it, reaching nowhere near the corners - so a ball the size of the card is far too big, and in a
//            corridor it fills the corridor.
static const float CloudFatness    = 0.55f;
//   FadeOut  the seconds a puff that has gone is kept for, thinning. The game fades a dying puff through its own
//            colour, but it can also simply stop sending one, and a puff that blinks out is the one thing smoke never
//            does. A NEW puff is not eased in here: the game brings its own in from nothing, and a puff eased in a
//            second time is one whose first moments - a bullet's puff, the head of a spray - are never seen.
// Motion blur: how much of one frame's movement is smeared (1 is the whole of it - a shutter open the whole
// frame), the furthest any sample may reach whatever the vector says, and how many samples are taken. The
// limit matters more than it looks: a pixel that leaps across the screen - a cut, a teleport, the first frame
// after a load - would otherwise smear the whole picture into a streak.
static const float BlurStrengthDefault = 0.55f;
static const float BlurMaxPixels       = 64.0f;
static const float BlurTaps            = 8.0f;

// Depth of field, in a conversation's camera (DepthOfField.h). The largest circle is a fraction of the picture's height -
// 17 pixels at 2160, 9 at 1080 - so the look does not change with the resolution; the strength (1 by default, the marker
// SecondRevision-dof.txt in hundredths) scales it. The band is the part of |1 - focus / distance| that stays sharp: the
// person spoken to, a step behind the speaker, stays nearly so. The focus is never nearer than NearestFocus (a speaker
// standing inside the camera would ask for a circle everywhere), a new speaker is followed over about FocusSeconds, and
// the whole of it comes and goes over FadeSeconds as a conversation starts and ends.
static const float DofStrengthDefault = 1.0f;
static const float DofMaxCocOfHeight  = 0.008f;
static const float DofSharpBand       = 0.08f;
static const float DofNearestFocus    = 24.0f;
static const float DofFocusSeconds    = 0.25f;
static const float DofFadeSeconds     = 0.4f;
// How far in front of the speaker's eyes' place the focus is set. That place is on the body's axis - the middle of
// the head - and what the camera sees of the head is nearer: its side by less, the face by more. This lies between
// the two, so that in a close-up, where little stays sharp around the focus, both do.
static const float DofFaceLead        = 4.0f;
// The focus's distance for a speaker whose eyes' place lies Along units down the view.
inline float DofFocusFor(float Along, float Lead = DofFaceLead)
{
    const float Focus = Along - Lead;
    return (Focus > DofNearestFocus) ? Focus : DofNearestFocus;
}

// The lens and the film. Each is off at zero, and each is its own switch - they only share a pass because they
// share a PLACE: the finished picture, after the antialiasing, before the HUD.
//   Aberration  how far apart the colours are pulled at the very corner, in pixels of the finished picture.
//               Small: this is the one effect that goes from "a lens" to "a broken television" in one step.
//   Vignette    how much the corner is darkened. These levels are dark already; a little goes a long way.
//   Grain       how much the film's own unevenness shows.
static const float AberrationDefault   = 4.0f;
static const float VignetteDefault     = 0.28f;
static const float GrainDefault        = 0.05f;
//   Bloom       how much of the picture's halo is added back, and where "bright" starts. The threshold is what
//               keeps a dim corridor from glowing: under it a thing adds nothing at all. Spread is how far the blur
//               reaches, in pixels of the quarter-size picture it is done on.
static const float BloomDefault        = 0.55f;
static const float BloomThreshold      = 0.62f;
static const float BloomSpread         = 2.0f;
//   Coronas     the second halo, of the engine's coronas alone, added to the picture's with a weight of its own. A
//               corona is a soft glow already, from its rim to its middle, so its line is lower than the picture's
//               and only its bright middle goes past it; and it is large on the screen - hundreds of pixels in 4K -
//               so the blur reaches much further: a wider step, and the whole blur twice, which widens the bell
//               without gaps between the taps.
static const float BloomCoronaDefault   = 1.0f;
static const float BloomCoronaThreshold = 0.25f;
static const float BloomCoronaSpread    = 5.0f;
static const int   BloomCoronaRounds    = 2;
//   Lens flare  how much of the ghosts is added. They are made of the coronas' own picture, over this line - the
//               corona's bright middle, not its soft rim - so a lamp the engine shows no corona for throws none.
static const float FlareDefault        = 0.45f;
static const float FlareSpread         = 4.0f;
static const float FlareCoronaLine     = 0.35f;
//   Waterfalls  Water=Low and up (BLEND_Fall; SolidPass.hlsl, FallSurface): the water's hue over what is seen through
//               a fall, how white its streams are, how far they bend the view (the level's units at the sheet), and
//               how fast they run down (repeats of the texture a second, on top of the engine's own frames).
//               The hue is the water's the fall comes down into (its zone's view fog, as the water's surface takes
//               it); these are the murk's, for a fall with no water under it.
static const float WaterfallHueR         = 0.62f;
static const float WaterfallHueG         = 0.58f;
static const float WaterfallHueB         = 0.46f;
static const float WaterfallFoam         = 0.25f;
static const float WaterfallBend         = 2.00f;
static const float WaterfallSpeed        = 0.60f;
//               How much of what is behind the falling water swallows where it is thickest (its murk), and how far
//               down under a sheet's lowest corner its water is looked for.
static const float WaterfallDensity      = 3.00f;
static const float WaterfallWaterSearch  = 1024.0f;
//               The foot, where a fall comes down into water: a splash from each foot every WaterfallSplashEvery
//               seconds (give or take), as a body of WaterfallSplashRadius falling at WaterfallSplashSpeed would throw
//               (SplashMax is shared with everything else that falls in); and the ripples' push there, every frame.
static const float WaterfallSplashEvery  = 0.80f;
static const float WaterfallSplashRadius = 10.0f;
static const float WaterfallSplashSpeed  = 800.0f;
//               How fast a fall's splash throws its column up against a body's: its height goes by the square of it,
//               so 0.71 is half as high; the crown and the spray are a body's.
static const float WaterfallSplashColumn = 0.71f;
//               The rings a foot sends out: a push WaterfallRippleRadius across at its middle, rising and falling
//               WaterfallRippleRate times a second, WaterfallRipplePush at its strongest - in time, so the rings are
//               rings (the ripples run about 80 units a second, so 1.6 a second lays them some 50 units apart), and
//               both ways, so nothing piles up under the foot, as a push only ever one way does. The field run on its
//               own with this push: the rings' slope 0.13 a metre (52 units) out, 0.06 at two, 0.03 at three - gone
//               by then, 1.5 units tall at a metre.
static const float WaterfallRippleRadius = 24.0f;
static const float WaterfallRipplePush   = 0.06f;
static const float WaterfallRippleRate   = 1.6f;
//               As a volume (FVolumetrics, in place of the sheets): where sheets cross, a round column as wide as
//               WaterfallColumnWidth of their half-width; a sheet on its own, a ribbon WaterfallRibbonWidth of its
//               half-width and WaterfallRibbonThick of that thick, at least WaterfallRibbonMin units. How fast the view
//               closes up in its thickest water, per unit - light enough that the streaks show, not a solid black
//               pillar - and the light it gives back: the water's hue times WaterfallAlbedo, the foam's
//               WaterfallFoamLight, the glint's WaterfallGlint of the lamp; how many streaks a second go by a point of it.
static const float WaterfallColumnWidth  = 0.75f;
static const float WaterfallRibbonWidth  = 0.85f;
static const float WaterfallRibbonThick  = 0.20f;
static const float WaterfallRibbonMin    = 4.0f;
static const float WaterfallVolumeDensity = 0.06f;
static const float WaterfallAlbedo       = 0.004f;
static const float WaterfallAmbient      = 1.0f;
static const float WaterfallFoamLight    = 0.03f;
static const float WaterfallGlint        = 0.02f;
static const float WaterfallStreakRate   = 6.0f;
//               The temporal upscaler's reactive mask where the walk lays a column, whose streaks are somewhere else
//               every frame and have no motion vectors: the share of the column's cover of a pixel that is told, 0 for
//               nothing told (as the smoke's, CloudReactive). All of it: untold, an upscaler that reads the mask
//               blends the streaks of several frames and the column all but stands; the wall beside it and the picture
//               in a turn are the same either way (docs/fsr3-atadas.md, 12.).
static const float WaterfallReactive     = 1.0f;
//   Fire          a fire's flames as bodies of flame (the Fire3D key; docs/terv-3d-tuz.md). A fire standing on its
//               foot is at least FireBodyMinHalf units from its middle across; the light it gives off per unit of the
//               ray at its hottest, FireGlow (the heat's own colour, Planck's, in the walk), tinted a little by
//               FireTint; its grain rising FireRise repeats a second - with the walk's 0.07 repeats a unit upwards
//               that is 50 units, about a metre, a second.
static const float FireBodyMinHalf       = 6.0f;
static const float FireGlow              = 0.3f;
static const float FireTintR             = 1.0f;
static const float FireTintG             = 0.62f;
static const float FireTintB             = 0.3f;
static const float FireRise              = 4.0f;
//   How far from its sheets the walk shows a flame - the walk takes these as its macros (Volumetrics.cpp) - and so
//   what the tiles a body is listed in go by (FlameBodyBall). A standing fire's tongues reach FireWalkReach times its
//   sheets' height up from its foot; a fireball's heat is read out to FireBallOut of its radius across, and
//   FireBallTall times that up and down; a standing fire's sparks fly no further from its top than its own width and
//   height and, for a fire of the size the sparks were shaped on, FireSparkFly units.
static const float FireWalkReach         = 1.8f;
static const float FireBallOut           = 1.45f;
static const float FireBallTall          = 1.3f;
static const float FireSparkFly          = 96.0f;
//   A fire's flame CARD (the game's burning floors and people: Effects.Fire's flame_b, OneFlame_J, OnFire_J) as a flame
//   body: the flame painted on it is narrower than the card - FireCardWidth of it - and stands on the card's lower
//   edge; the body is FireCardTall of the card's height, and its tongues reach on above that (FIRE_REACH, the walk).
static const float FireCardWidth         = 0.7f;
static const float FireCardTall          = 0.55f;
//   A mesh's flame planes are the flame's own outline, not a card with a picture on it: the body is as tall as they
//   are.
static const float FireMeshTall          = 1.0f;
//   A small body in the walk. The walk reads the level at the middles of its target's pixels: a body smaller than a
//   pixel or two is met by a ray or missed by it as it moves, and what it adds to the picture comes and goes. So a
//   body is never walked smaller than a few pixels: one that is, is widened to Least across - round its upright axis -
//   and to Least up, and holds by the unit as much less as it grew, so that all it adds to the picture stays what it
//   was. Across and Up go in as the body's own half-sizes and come out widened; the answer is what its strength by
//   the unit - a flame's glow, a puff's thickness - is multiplied by.
inline float VolumeWiden(float& Across, float& Up, float Least)
{
    float Keep = 1.0f;
    if (Across < Least)
    {
        const float Of = ((Across > 0.0f) ? Across : 0.0f) / Least;
        Keep  *= Of * Of;
        Across = Least;
    }
    if (Up < Least)
    {
        Keep *= ((Up > 0.0f) ? Up : 0.0f) / Least;
        Up    = Least;
    }
    return Keep;
}
//   One tongue of flame. A flame narrower than FireTongueHalf from its middle - a candle's, two or three units across -
//   is not a fire with tongues of its own: the grain that tears a fire's top into tongues is coarser than the whole of
//   it. It is walked as a single tongue of its own size, in the shape of the game's picture of one (OneFlame_J): the
//   flame fills FireCardWidth of its sheet or card across and FireTongueTall of it upwards, round its middle.
static const float FireTongueHalf        = 4.0f;
static const float FireTongueTall        = 0.9f;
//   The game paints its flames alike whatever their size, and the walk's light goes by how much flame the ray passes
//   through. So a tongue glows, by the unit, as many times stronger as it is narrower than FireTongueHalf: through
//   its middle a candle's flame is as bright as a large tongue's. FireTongueGainMost at the most.
static const float FireTongueGainMost    = 16.0f;
inline float FireTongueGain(float Half)
{
    if (!(Half < FireTongueHalf))
        return 1.0f;
    const float Least = FireTongueHalf / FireTongueGainMost;
    return FireTongueHalf / ((Half > Least) ? Half : Least);
}
//   A level's lone tongue - a candle's flame, a torch's - has no fire round it to add to its glow, and the game
//   paints one white-hot through its middle, a bright point from far off: it glows FireTongueCore times what the
//   rule above gives it. Weaker, and where the walk hands the flame back to the game's own picture (FireMinPixels)
//   the same candle is seen to change.
static const float FireTongueCore        = 5.0f;
//   A large fire. The walk's flame was shaped on fires a fireplace's size, and its light goes by how much flame the
//   ray passes through: a blaze ten times as deep would be ten times as bright - white from end to end - and made of
//   the same small tongues, hundreds of them. So a body wider than FireScaleHalf from its middle, the narrow way, is
//   that flame made larger: its grain as many times coarser as the body is wider than that, and its glow as many
//   times thinner by the unit. Through its middle a blaze is then as bright as a fire FireScaleHalf across, and its
//   tongues are as large to it as a fireplace's are to a fireplace.
static const float FireScaleHalf         = 24.0f;
inline float FireBodyScale(float HalfU, float HalfV)
{
    const float Narrow = (HalfU < HalfV) ? HalfU : HalfV;
    return (Narrow > FireScaleHalf) ? Narrow / FireScaleHalf : 1.0f;
}
//   The walk's target is half the scene's size across. A candle's sheet whose flame is narrower on it than
//   FireMinPixels is left to the game: its picture is still a picture at that size, while the walk would catch the
//   flame with a pixel or two - and a chapel's hundred candles seen from its far end are not a hundred bodies to
//   walk. A sheet already walked is kept down to FireKeepPixels, so that one standing at the very distance does not
//   change from the one to the other as the view sways. Only a level's sheets small enough to be a tongue are asked
//   this: a larger fire, a card and a mesh's flame are walked wherever they are.
static const float FireMinPixels         = 4.0f;
static const float FireKeepPixels        = 3.2f;
//   A tongue that is walked is never narrower on the target than FireTongueMinPixels, nor lower: one further off is
//   widened to that and glows the weaker for it by as much as it grew - the same light over more pixels - so a small
//   flame far away dims as a far light does instead of flickering between the walk's pixels. UnitsAPixel: how many
//   of the level's units one pixel of the target covers where the tongue stands. Half and HalfTall go in as the
//   tongue's own and come out widened; the answer is how many times weaker it glows by the unit.
static const float FireTongueMinPixels   = 3.0f;
inline float FireTongueWiden(float& Half, float& HalfTall, float UnitsAPixel)
{
    return VolumeWiden(Half, HalfTall, 0.5f * FireTongueMinPixels * UnitsAPixel);
}
// How wide, in the walk target's pixels, something Wide units across shows at Depth into the view: Focal and ViewWide
// are the engine's projection and its view's width in its own pixels, TargetWide the walk target's width.
inline float FlamePixels(float Wide, float Depth, float Focal, float ViewWide, int TargetWide)
{
    if (!(ViewWide > 0.0f) || TargetWide <= 0)
        return 0.0f;
    if (!(Depth > 1.0f))
        return 1.0e9f;      // at the eye's own plane: as large as anything gets
    return Wide * Focal / Depth * (float)TargetWide / ViewWide;
}

//   Which textures are flames to walk. The group Effects.Fire is NOT the list: beside the flames it holds the sparks
//   (SparkFX1, Spark_Electric), the plasma rifle's glow, its bolt and the bolt's trail (Wepn_Prifle_SFX, OneFlame_G,
//   proj_Prifle - green), the rockets' exhaust (Fireball1, FireballWhite) and the flamethrower's stream and pilot
//   flame (flmethrwr_fire, flmethrwr_flme). So the flames are named, each for the shape of its own picture
//   (scripts/content_query.py textures effects.fire):
//
//     Effects.Fire.flame_b, Flame_J   a fire standing on its foot: a fireplace's sheets, a burning floor's cards, the
//                                     burning barrel's flame planes
//     HDTPanim.BarrelFire.*           HDTP's burning barrel: the frames of its flame
//     Effects.Fire.OnFire_J           the fireball on a burning body
//     Effects.Fire.OneFlame_J         one tongue of flame: the licks of a burning body, a candle's and a torch's flame
//
//   Anything else - a texture some other package adds to the group among them - stays the game's own picture.
enum EFlame
{
    Flame_None = 0,
    Flame_Standing,
    Flame_Ball,
    Flame_Lick
};

// Whether Name begins with Want, in any case - and with bWhole, ends there too. Want is in small letters.
template <typename CH>
inline bool FlameNameIs(const CH* Name, const char* Want, bool bWhole)
{
    int n = 0;
    for (; Want[n] != 0; n++)
    {
        // Reading past the end of the name cannot happen: its closing zero is no letter of the word.
        CH Ch = Name[n];
        if (Ch >= (CH)'A' && Ch <= (CH)'Z')
            Ch = (CH)(Ch - (CH)'A' + (CH)'a');
        if (Ch != (CH)Want[n])
            return false;
    }
    return !bWhole || Name[n] == (CH)0;
}

// The name as the engine spells it, Package.Group.Name (a template over the character type, as IsCloudTexture).
template <typename CH>
inline EFlame FlameKind(const CH* PathName)
{
    if (!PathName)
        return Flame_None;
    if (FlameNameIs(PathName, "effects.fire.flame_b", true) || FlameNameIs(PathName, "effects.fire.flame_j", true) ||
        FlameNameIs(PathName, "hdtpanim.barrelfire.", false))
        return Flame_Standing;
    if (FlameNameIs(PathName, "effects.fire.onfire_j", true))
        return Flame_Ball;
    if (FlameNameIs(PathName, "effects.fire.oneflame_j", true))
        return Flame_Lick;
    return Flame_None;
}

// A level surface that wears a flame is a flame's sheet where it is drawn as the game draws its fire - see-through,
// adding its light to what is behind it - and where it stands: a body of flame is as wide and as tall as its sheet,
// and a sheet lying flat has no height to give it. A flame painted on a solid wall, and a sheet laid on a floor,
// stay surfaces. PlaneZ: how far the surface's normal points up or down, of 1.
static const float FlameSheetLean = 0.7f;
inline bool FlameSheetStands(unsigned int PolyFlags, float PlaneZ)
{
    const float Up = (PlaneZ < 0.0f) ? -PlaneZ : PlaneZ;
    return BlendFor(PolyFlags) == Blend_Translucent && Up < FlameSheetLean;
}

// Whether two pieces of flame stand together, one body of flame: their middles on the level within their half-widths
// of one another, and - a level's sheets, as a fireplace's few are - sharing a quarter of the lower one's height;
// where either is round (a card, a mesh's polygon: Half every way), overlapping in height at all.
inline bool FlamesJoin(const float* MidA, float HalfA, float BottomA, float TopA, bool bRoundA,
                       const float* MidB, float HalfB, float BottomB, float TopB, bool bRoundB)
{
    const float dx    = MidA[0] - MidB[0];
    const float dy    = MidA[1] - MidB[1];
    const float Reach = HalfA + HalfB;
    if (dx * dx + dy * dy > Reach * Reach)
        return false;
    const float Shared = ((TopA < TopB) ? TopA : TopB) - ((BottomA > BottomB) ? BottomA : BottomB);
    if (bRoundA || bRoundB)
        return Shared >= 0.0f;
    const float TallA = TopA - BottomA;
    const float TallB = TopB - BottomB;
    return Shared >= 0.25f * ((TallA < TallB) ? TallA : TallB);
}

// The bodies counted while the pieces still come, one at a time: Group says which body each piece so far stands in -
// a chain of pieces ending in one that names itself, the body's root. FlameRoot follows a piece's chain (and
// shortens it); FlameBodyAdd takes piece number Count in, standing with the StandCount earlier pieces listed in
// Stands - none: a body of its own; several bodies: they are one from here on - and keeps Bodies right.
inline int FlameRoot(int* Group, int i)
{
    while (Group[i] != i)
        i = Group[i] = Group[Group[i]];
    return i;
}
inline void FlameBodyAdd(int* Group, int Count, int& Bodies, const int* Stands, int StandCount)
{
    Group[Count] = Count;
    Bodies++;
    for (int k = 0; k < StandCount; k++)
    {
        const int A = FlameRoot(Group, Stands[k]);
        if (A != Count)
        {
            Group[A] = Count;
            Bodies--;
        }
    }
}

// A piece of a fire's flame, gathered by the draw path instead of drawn - a level's flame sheet, a flame card, a
// mesh's flame polygon: its middle on the level, the direction across it and its half-width that way, how far down
// and up it reaches.
//   bRound    a card's or a mesh polygon's piece: a card faces the camera, so it says nothing about which way its
//             flame lies - Half every way, and Across left to the world's X.
//   Kind      which of the game's flames it stands for (FlameShape_*), walked each in the shape of the game's own
//             picture of it; a piece joins only others of its kind.
//   Bright    the game's own fade of the card, 1 for a sheet or a mesh.
//   Tongue    the one tongue of flame painted on the piece - its half-width, its foot and its top: what a fire too
//             small for a standing fire's shape is walked as (FireTongueHalf).
//   Depth     how far into the view the piece stands.
//   Tint      what colours its glow: the flames' own (FireTintR, G, B), or a glow's picture's colour.
// FlameShape_Glow: no flame at all but a ball of lit gas that burns nothing and rises nowhere (FlameGlowPiece) -
// walked with the flames, since it is light added as theirs is. FlameShape_GlowPart: a polygon of a mesh of glow
// (FlameGlowMeshPiece) - a piece only: those that touch make one body of glow, and no body is of this kind.
enum { FlameShape_Standing = 0, FlameShape_Ball = 1, FlameShape_Lick = 2, FlameShape_Glow = 3,
       FlameShape_GlowPart = 4 };
struct FFlamePiece
{
    float Mid[2];
    float Across[2];
    float Half;
    float Bottom, Top;
    int   bRound;
    int   Kind;
    float Bright;
    float Tongue[3];
    float Depth;
    float Tint[3];
    // A glow's alone: how white its picture's own brightest colour is, of 1 - its densest part turns that far towards
    // white and no further. The electric sparks' palette ends in white; the plasma bolt's is green to its last entry.
    float White;
};
// How bright a ball of glow is seen through its middle, in what the walk adds up by the unit across a ball one unit
// in radius (FlameBodiesMake).
static const float FireGlowThrough = 0.6f;
// A ball of glow whose middle lies within this much of another's half-width of that one's middle, and that is no
// more than half as wide again or as narrow, is that other ball (FFlameGather::Add).
static const float FireGlowMerge = 0.35f;
// The flames' own tint on a piece.
inline void FlameOwnTint(FFlamePiece& P)
{
    P.Tint[0] = FireTintR;
    P.Tint[1] = FireTintG;
    P.Tint[2] = FireTintB;
    P.White   = 1.0f;
}
// How many numbers a body of flame is to the walk (FVolumetrics::FIRE_FLOATS).
enum { FlameBodyFloats = 16 };

// A level's flame sheet as a piece: its middle on the level, the level's direction across it and its half-width that
// way, its foot and its top. The tongue painted on it is narrower than the sheet and not quite as tall.
inline FFlamePiece FlameSheetPiece(float MidX, float MidY, float AcrossX, float AcrossY, float Half, float Bottom,
                                   float Top, float Depth)
{
    const float Tall = Top - Bottom;
    const float MidZ = 0.5f * (Top + Bottom);
    FFlamePiece P;
    P.Mid[0]    = MidX;
    P.Mid[1]    = MidY;
    P.Across[0] = AcrossX;
    P.Across[1] = AcrossY;
    P.Half      = Half;
    P.Bottom    = Bottom;
    P.Top       = Top;
    P.bRound    = 0;
    P.Kind      = FlameShape_Standing;
    P.Bright    = 1.0f;
    P.Tongue[0] = Half * FireCardWidth;
    P.Tongue[1] = MidZ - 0.5f * FireTongueTall * Tall;
    P.Tongue[2] = MidZ + 0.5f * FireTongueTall * Tall;
    P.Depth     = Depth;
    FlameOwnTint(P);
    return P;
}
// A mesh's flame polygon as a piece: the box of its points in the level, from Lo to Hi - a round one, since a
// polygon of a mesh that turns with its actor says nothing lasting about which way the flame lies.
inline FFlamePiece FlameMeshPiece(float LoX, float LoY, float LoZ, float HiX, float HiY, float HiZ, float Depth)
{
    const float WideX = HiX - LoX, WideY = HiY - LoY;
    FFlamePiece P;
    P.Mid[0]    = 0.5f * (LoX + HiX);
    P.Mid[1]    = 0.5f * (LoY + HiY);
    P.Across[0] = 1.0f;
    P.Across[1] = 0.0f;
    P.Half      = 0.5f * ((WideX > WideY) ? WideX : WideY) * FireCardWidth;
    P.Bottom    = LoZ;
    P.Top       = LoZ + (HiZ - LoZ) * FireMeshTall;
    P.bRound    = 1;
    P.Kind      = FlameShape_Standing;
    P.Bright    = 1.0f;
    P.Tongue[0] = P.Half;
    P.Tongue[1] = P.Bottom;
    P.Tongue[2] = P.Top;
    P.Depth     = Depth;
    FlameOwnTint(P);
    return P;
}
// No floor known under a flame card: lower than any card's foot.
static const float FireNoFloor = -1.0e30f;
// A flame card as a piece: which flame its picture is, the card's middle in the level, how wide and how tall it is
// there, and the game's own fade of it. Floor: how high the level stands under a standing fire's card. The game lights
// a fire on the ground as a card with its MIDDLE on the ground - the lower half of it sunk in the floor - and what
// shows of it is the part above; so is the flame's body only that part of the card, standing on the floor. A floor at
// or under the card's lower edge (FireNoFloor) leaves the whole card the flame.
inline FFlamePiece FlameCardPiece(EFlame Flame, float MidX, float MidY, float MidZ, float Wide, float Tall,
                                  float Bright, float Depth, float Floor = FireNoFloor)
{
    FFlamePiece P;
    P.Mid[0]    = MidX;
    P.Mid[1]    = MidY;
    P.Across[0] = 1.0f;
    P.Across[1] = 0.0f;
    P.bRound    = 1;
    P.Bright    = Bright;
    P.Depth     = Depth;
    FlameOwnTint(P);
    if (Flame == Flame_Ball)
    {
        // A fireball: its picture a ball over most of the card, round the card's middle.
        P.Kind      = FlameShape_Ball;
        P.Half      = 0.45f * Wide;
        P.Bottom    = MidZ - 0.4f * Tall;
        P.Top       = MidZ + 0.4f * Tall;
        P.Tongue[0] = P.Half;
        P.Tongue[1] = P.Bottom;
        P.Tongue[2] = P.Top;
    }
    else if (Flame == Flame_Lick)
    {
        // A lick: a drop of flame from the card's foot nearly to its top, a third of its width.
        P.Kind      = FlameShape_Lick;
        P.Half      = 0.35f * Wide;
        P.Bottom    = MidZ - 0.45f * Tall;
        P.Top       = MidZ + 0.45f * Tall;
        P.Tongue[0] = P.Half;
        P.Tongue[1] = P.Bottom;
        P.Tongue[2] = P.Top;
    }
    else
    {
        const float Lower = MidZ - 0.5f * Tall;
        const float Upper = MidZ + 0.5f * Tall;
        const bool  bSunk = Floor > Lower && Floor < Upper;
        const float Foot  = bSunk ? Floor : Lower;
        const float Seen  = bSunk ? Upper - Floor : Tall;
        const float Mid   = bSunk ? 0.5f * (Floor + Upper) : MidZ;
        P.Kind      = FlameShape_Standing;
        P.Half      = 0.5f * Wide * FireCardWidth;
        P.Bottom    = Foot;
        P.Top       = Foot + Seen * FireCardTall;
        P.Tongue[0] = P.Half;
        P.Tongue[1] = Mid - 0.5f * FireTongueTall * Seen;
        P.Tongue[2] = Mid + 0.5f * FireTongueTall * Seen;
    }
    return P;
}

// The game's glows that are no flame: the puffs of its electric sparks, what the plasma rifle's bolt trails and
// throws where it hits, and the plasma's own fire that burns on where a bolt struck - DeusEx.PlasmaFire, whose card
// wears the picture named for the flamethrower's pilot flame (Effects.Fire.Spark_Electric, proj_Prifle,
// flmethrwr_flme). Named, as the flames are (FlameKind): the group holds much else. With the 3D fire each card of
// them is a ball of glow in the walk (FlameGlowPiece).
template <typename CH>
inline bool GlowName(const CH* PathName)
{
    return PathName && (FlameNameIs(PathName, "effects.fire.spark_electric", true) ||
                        FlameNameIs(PathName, "effects.fire.proj_prifle", true) ||
                        FlameNameIs(PathName, "effects.fire.flmethrwr_flme", true));
}
// A card of glow as a piece: a ball round the card's middle in the level, of the picture's own colour (Hue). The
// walk's ball of glow is gone some seven tenths of the way out from its middle, so the piece is that much wider than
// the picture on the card - Fill of the card across. The game's fade of the card is its brightness.
inline FFlamePiece FlameGlowPiece(float MidX, float MidY, float MidZ, float Wide, float Tall, float Fill,
                                  float Bright, float Depth, const float* Hue, float White = 1.0f)
{
    const float Half = 0.7f * Fill * ((Wide > Tall) ? Wide : Tall);
    FFlamePiece P;
    P.Mid[0]    = MidX;
    P.Mid[1]    = MidY;
    P.Across[0] = 1.0f;
    P.Across[1] = 0.0f;
    P.Half      = Half;
    P.Bottom    = MidZ - Half;
    P.Top       = MidZ + Half;
    P.bRound    = 1;
    P.Kind      = FlameShape_Glow;
    P.Bright    = Bright;
    P.Tongue[0] = Half;
    P.Tongue[1] = P.Bottom;
    P.Tongue[2] = P.Top;
    P.Depth     = Depth;
    P.Tint[0]   = Hue[0];
    P.Tint[1]   = Hue[1];
    P.Tint[2]   = Hue[2];
    P.White     = White;
    return P;
}

// A ball of glow against a wall of the level. The game throws its cards of glow from where a bolt struck, their
// middles on the wall or in it; a card is cut by the wall along a line, and so is a ball - through its middle, where
// it is densest, and from along the wall that is half a ball ending in a bright edge on every ridge of the wall. So
// a ball stands clear of the wall: its middle at least GlowWallClear of its reach before the wall's face, and
// GlowWallRelief more for what a relief raises that face - what the wall then crosses of it is its thin rim.
// Dist: how far before the wall's face the ball's middle lies along the wall's normal, below nought behind it; Half
// the ball's reach. The answer is how far out along the normal the middle is moved. Keep (out): what is left of its
// light - all of it before the wall, less the deeper its middle lies in the wall, none once it is wholly inside,
// where the wall hid the card too.
static const float GlowWallClear  = 0.75f;
static const float GlowWallRelief = 2.0f;
inline float GlowWallPush(float Dist, float Half, float& Keep)
{
    Keep = 1.0f;
    const float Want = GlowWallClear * Half + GlowWallRelief;
    if (!(Half > 0.0f) || Dist >= Want)
        return 0.0f;
    if (Dist <= -Half)
    {
        Keep = 0.0f;
        return 0.0f;
    }
    if (Dist < 0.0f)
        Keep = 1.0f + Dist / Half;
    return Want - Dist;
}

// Whether a card of glow is a tongue and no round glow: one much taller than it is wide - the plasma's fire, whose
// picture is twice as tall as it is wide. Then the body is no ball as large as the card is tall, but one drawn out
// to the card's own width and height: Thin its half-width, High its half-height. By the card alone, which is the
// same every frame: the game works these pictures out afresh each frame, and a body shaped by what a picture
// happens to show changes its shape with it - in a game that stands still, too.
inline bool GlowTongue(float Wide, float Tall, float& Thin, float& High)
{
    Thin = 0.45f * Wide;
    High = 0.45f * Tall;
    return Tall > 1.5f * Wide;
}

// A mesh of glow: the plasma rifle's bolt, whose mesh wears Effects.Fire.OneFlame_G. Named, as GlowName's.
template <typename CH>
inline bool GlowMeshName(const CH* PathName)
{
    return PathName && FlameNameIs(PathName, "effects.fire.oneflame_g", true);
}
// A polygon of such a mesh as a piece: the box of its points in the level, from Lo to Hi - its half-widths along
// the level's X and Y ride in the tongue's first two numbers, which a piece of glow has no other use for. The
// pieces of one mesh touch, and are one body (FlameBodiesMake).
inline FFlamePiece FlameGlowMeshPiece(float LoX, float LoY, float LoZ, float HiX, float HiY, float HiZ, float Depth,
                                      const float* Hue, float White = 1.0f)
{
    const float HalfX = 0.5f * (HiX - LoX), HalfY = 0.5f * (HiY - LoY);
    FFlamePiece P;
    P.Mid[0]    = 0.5f * (LoX + HiX);
    P.Mid[1]    = 0.5f * (LoY + HiY);
    P.Across[0] = 1.0f;
    P.Across[1] = 0.0f;
    P.Half      = (HalfX > HalfY) ? HalfX : HalfY;
    P.Bottom    = LoZ;
    P.Top       = HiZ;
    P.bRound    = 1;
    P.Kind      = FlameShape_GlowPart;
    P.Bright    = 1.0f;
    P.Tongue[0] = HalfX;
    P.Tongue[1] = HalfY;
    P.Tongue[2] = HiZ;
    P.Depth     = Depth;
    P.Tint[0]   = Hue[0];
    P.Tint[1]   = Hue[1];
    P.Tint[2]   = Hue[2];
    P.White     = White;
    return P;
}

// Whether two pieces are of one body of flame. A fireball or a lick is a flame of its own - the game puts each where
// it burns, and the pattern of them is the burning body's - and a flame of one kind never joins one of another. A
// mesh of glow's polygons that touch are one body of glow.
inline bool FlamePiecesJoin(const FFlamePiece& A, const FFlamePiece& B)
{
    if (A.Kind == FlameShape_GlowPart && B.Kind == FlameShape_GlowPart)
        return FlamesJoin(A.Mid, A.Half, A.Bottom, A.Top, true, B.Mid, B.Half, B.Bottom, B.Top, true);
    return A.Kind == FlameShape_Standing && B.Kind == FlameShape_Standing &&
           FlamesJoin(A.Mid, A.Half, A.Bottom, A.Top, A.bRound != 0, B.Mid, B.Half, B.Bottom, B.Top, B.bRound != 0);
}

// Which fire each of a level's flame sheets stands in: the sheets that stand together, and those that stand with
// them in turn, are one fire - a fireplace's two, the long blaze of a wreck's several. Fire gets, for every piece,
// the number of its fire, the fires numbered in the order of their first pieces; the answer is how many there are.
// The level's own, whichever of the sheets a frame draws: a fire worked out from what a frame happened to draw
// changed its shape as its sheets came into the view.
inline int FlameFiresOf(const FFlamePiece* Pieces, int Count, int* Fire)
{
    for (int i = 0; i < Count; i++)
        Fire[i] = i;
    for (int i = 0; i < Count; i++)
        for (int j = i + 1; j < Count; j++)
            if (FlamePiecesJoin(Pieces[i], Pieces[j]))
            {
                // The lower root stays: a fire is named by its first piece.
                const int A = FlameRoot(Fire, i), B = FlameRoot(Fire, j);
                if (A < B)
                    Fire[B] = A;
                else if (B < A)
                    Fire[A] = B;
            }
    for (int i = 0; i < Count; i++)
        Fire[i] = FlameRoot(Fire, i);
    // A root is at or before every piece of its fire, so its number is there by the time a later piece asks for it.
    int Fires = 0;
    for (int i = 0; i < Count; i++)
        Fire[i] = (Fire[i] == i) ? Fires++ : Fire[Fire[i]];
    return Fires;
}

// The frame's pieces of flame, in a list of the caller's (Hold). There is no most flames a frame may hold: the game
// lights as many as it does, and whoever gathers them makes the list longer before a piece would not fit. A flame
// left to the game's own picture for want of a place flips between that picture and the walked one as the flames in
// view come to more and to fewer than the list holds.
struct FFlameGather
{
    FFlamePiece* Pieces;
    int          Room;          // how many pieces the list holds
    int          Count;         // the pieces taken

    FFlameGather() : Pieces(nullptr), Room(0), Count(0) {}
    // The list, InRoom long: the one before made longer, what it held with it.
    void Hold(FFlamePiece* InPieces, int InRoom)
    {
        Pieces = InPieces;
        Room   = InRoom;
    }
    void Begin()
    {
        Count = 0;
    }
    // One piece - a flame card, a mesh's flame polygon. False only where the list is full: it is to be made longer
    // first.
    bool Add(const FFlamePiece& P)
    {
        auto Same = [](float A, float B, float Within) -> bool { return A - B < Within && B - A < Within; };
        // A ball of glow all but inside another one of its colour is that other ball, as bright as the brighter of
        // the two - where balls lie over one another the walk shows the densest of them, so nothing is lost -, and
        // not a ball more: a burst of sparks throws many from one spot, and what the walk costs a pixel goes by how
        // many balls it is asked about there.
        if (P.Kind == FlameShape_Glow)
            for (int i = 0; i < Count; i++)
            {
                FFlamePiece& Q = Pieces[i];
                if (Q.Kind != FlameShape_Glow || !(P.Half < 1.5f * Q.Half) || !(Q.Half < 1.5f * P.Half) ||
                    !Same(Q.Tint[0], P.Tint[0], 0.02f) || !Same(Q.Tint[1], P.Tint[1], 0.02f) ||
                    !Same(Q.Tint[2], P.Tint[2], 0.02f))
                    continue;
                const float Dx   = Q.Mid[0] - P.Mid[0], Dy = Q.Mid[1] - P.Mid[1];
                const float Dz   = 0.5f * ((Q.Bottom + Q.Top) - (P.Bottom + P.Top));
                const float Near = FireGlowMerge * ((Q.Half > P.Half) ? Q.Half : P.Half);
                if (Dx * Dx + Dy * Dy + Dz * Dz < Near * Near)
                {
                    if (P.Bright > Q.Bright)
                        Q.Bright = P.Bright;
                    return true;
                }
            }
        for (int i = 0; i < Count; i++)
        {
            const FFlamePiece& Q = Pieces[i];
            // The same piece again - the other triangle of a flame's quad - is one piece.
            if (Same(Q.Mid[0], P.Mid[0], 0.01f) && Same(Q.Mid[1], P.Mid[1], 0.01f) && Q.Kind == P.Kind &&
                (Q.bRound != 0) == (P.bRound != 0) && Same(Q.Half, P.Half, 0.01f) &&
                Same(Q.Bottom, P.Bottom, 0.01f) && Same(Q.Top, P.Top, 0.01f) &&
                Same(Q.Across[0], P.Across[0], 0.001f) && Same(Q.Across[1], P.Across[1], 0.001f))
                return true;
        }
        if (Count >= Room)
            return false;
        Pieces[Count++] = P;
        return true;
    }
    // A fire whose pieces are known together - a level's, all of its sheets (FlameFiresOf) - taken whole, in the
    // order given: its body is then the same whatever the frame draws of it. False where there is nothing to take, or
    // the list has no room for all of it - none of it is taken then.
    bool AddTogether(const FFlamePiece* P, int N)
    {
        if (N <= 0 || Count + N > Room)
            return false;
        for (int k = 0; k < N; k++)
            Pieces[Count++] = P[k];
        return true;
    }
};

// The bodies of flame the walk takes (FlameBodyFloats numbers each: the middle on the level and the foot and the top;
// the direction along the body and its half-length and half-width; its tint and a seed; which shape, how bright, how
// many times stronger it glows by the unit, how many times coarser its grain is) from a frame's pieces. The pieces
// that stand together (FlamesJoin) - a fireplace's few sheets, a barrel's planes - make one body: its footprint the
// box of all their ends, at least FireBodyMinHalf across, so a single flat sheet still makes a flame with a round to
// it. The box lies along one of the body's sheets: the one along which the body, as it is walked, is the smallest -
// of equal ones the first. Not along whichever came last: the engine sends a fire's sheets in an order that goes by
// where the camera stands, and the body would turn as the camera moved. Cards and a mesh's polygons say nothing of
// direction; a body of those alone lies along the world's X. A large body is the same flame made larger
// (FireBodyScale).
// A fire narrower than FireTongueHalf is one tongue of flame of its own size (the pieces' Tongue), never smaller on
// the walk's target than a few pixels (FireTongueWiden; UnitsAPixel: the level's units a pixel of the target covers
// at a depth of one). A fireball or a lick is a flame of its own. At most MaxOut bodies, the room Out has: a body a
// piece is always enough. Group: room for a number a piece, the caller's - there is no most pieces a frame may
// hold.
inline int FlameBodiesMake(const FFlamePiece* Pieces, int Count, float* Out, int MaxOut, float UnitsAPixel,
                           int* Group)
{
    auto Lesser  = [](float A, float B) -> float { return (A < B) ? A : B; };
    auto Greater = [](float A, float B) -> float { return (A > B) ? A : B; };
    for (int i = 0; i < Count; i++)
        Group[i] = i;
    for (int i = 0; i < Count; i++)
        for (int j = i + 1; j < Count; j++)
            if (FlamePiecesJoin(Pieces[i], Pieces[j]))
                Group[FlameRoot(Group, i)] = FlameRoot(Group, j);
    // How large the body rooted at r is, as it is walked, where its box lies along (Ux, Uy): half its length times
    // half its width.
    auto WalkedSize = [&](int r, float Ux, float Uy) -> float
    {
        const float Vx = -Uy, Vy = Ux;
        float U0 = 1e9f, U1 = -1e9f, V0 = 1e9f, V1 = -1e9f;
        for (int m = 0; m < Count; m++)
        {
            if (FlameRoot(Group, m) != r)
                continue;
            const FFlamePiece& P = Pieces[m];
            for (int e = -1; e <= 1; e += 2)
            {
                const float X = P.Mid[0] + P.Across[0] * P.Half * (float)e;
                const float Y = P.Mid[1] + P.Across[1] * P.Half * (float)e;
                const float u = X * Ux + Y * Uy, v = X * Vx + Y * Vy;
                U0 = Lesser(U0, u); U1 = Greater(U1, u);
                V0 = Lesser(V0, v); V1 = Greater(V1, v);
            }
            if (P.bRound)
            {
                const float v = P.Mid[0] * Vx + P.Mid[1] * Vy;
                V0 = Lesser(V0, v - P.Half); V1 = Greater(V1, v + P.Half);
            }
        }
        const float HalfU = Greater(0.5f * (U1 - U0), FireBodyMinHalf);
        return HalfU * Greater(Greater(0.5f * (V1 - V0), 0.4f * HalfU), FireBodyMinHalf);
    };
    int N = 0;
    for (int r = 0; r < Count && N < MaxOut; r++)
    {
        if (FlameRoot(Group, r) != r)
            continue;
        float* const F = Out + N * FlameBodyFloats;
        F[8]  = FireTintR;
        F[9]  = FireTintG;
        F[10] = FireTintB;
        F[14] = 1.0f;       // how many times stronger it glows by the unit: more for a small tongue (FireTongueGain),
        F[15] = 1.0f;       // less for a large fire, whose grain is as many times coarser (FireBodyScale)
        if (Pieces[r].Kind == FlameShape_GlowPart)
        {
            // A mesh of glow: one body as long, as wide and as tall as the box of all its polygons that touch -
            // along the level's own X and Y, which is all a box of boxes says -, a ball of glow drawn out to it.
            float X0 = 1e9f, X1 = -1e9f, Y0 = 1e9f, Y1 = -1e9f, Z0 = 1e9f, Z1 = -1e9f, Bright = 0.0f;
            for (int m = 0; m < Count; m++)
            {
                if (FlameRoot(Group, m) != r)
                    continue;
                const FFlamePiece& P = Pieces[m];
                X0 = Lesser(X0, P.Mid[0] - P.Tongue[0]); X1 = Greater(X1, P.Mid[0] + P.Tongue[0]);
                Y0 = Lesser(Y0, P.Mid[1] - P.Tongue[1]); Y1 = Greater(Y1, P.Mid[1] + P.Tongue[1]);
                Z0 = Lesser(Z0, P.Bottom);               Z1 = Greater(Z1, P.Top);
                Bright = Greater(Bright, P.Bright);
            }
            const float Up = Greater(0.5f * (Z1 - Z0), 2.0f);
            F[0]  = 0.5f * (X0 + X1);
            F[1]  = 0.5f * (Y0 + Y1);
            F[2]  = 0.5f * (Z0 + Z1) - Up;
            F[3]  = 0.5f * (Z0 + Z1) + Up;
            F[4]  = 1.0f;
            F[5]  = 0.0f;
            F[6]  = Greater(0.5f * (X1 - X0), 2.0f);
            F[7]  = Greater(0.5f * (Y1 - Y0), 2.0f);
            F[8]  = Pieces[r].Tint[0];
            F[9]  = Pieces[r].Tint[1];
            F[10] = Pieces[r].Tint[2];
            F[11] = Bright;     // a body of glow has no seed: the game's fade of it, for the discharges in it
            F[12] = (float)FlameShape_Glow;
            F[13] = 1.0f;
            // As bright seen across its thinnest as a ball of that size: a bolt is seen from its side.
            F[14] = FireGlowThrough * Bright / Lesser(Lesser(F[6], F[7]), Up);
            F[15] = Pieces[r].White;    // how far its densest part turns towards white
            N++;
            continue;
        }
        if (Pieces[r].Kind != FlameShape_Standing)
        {
            const FFlamePiece& P = Pieces[r];
            const float Cz = 0.5f * (P.Bottom + P.Top);
            const float Up = Greater(0.5f * (P.Top - P.Bottom), 2.0f);
            F[0]  = P.Mid[0];
            F[1]  = P.Mid[1];
            F[2]  = Cz - Up;
            F[3]  = Cz + Up;
            F[4]  = 1.0f;
            F[5]  = 0.0f;
            F[6]  = F[7] = Greater(P.Half, 2.0f);
            F[8]  = P.Tint[0];
            F[9]  = P.Tint[1];
            F[10] = P.Tint[2];
            F[11] = 0.0f;       // its grain is in its own frame (the walk), so the same seed for every one
            F[12] = (float)P.Kind;
            F[13] = P.Bright;
            // A ball of glow seen through its middle adds what its card did, whatever its size: the walk adds a
            // glow up by the unit, so a large ball glows that much less by the unit. And as the card's own light
            // went by the game's fade of it, so does the ball's - its strength, not how dense it is: the walk's
            // glow falls steeply with the density, and a card half faded would be all but gone.
            if (P.Kind == FlameShape_Glow)
            {
                F[11] = P.Bright;       // no seed: the game's fade of it, for the discharges in it
                F[13] = 1.0f;
                F[14] = FireGlowThrough * P.Bright / F[6];
                F[15] = P.White;        // how far its densest part turns towards white
            }
            N++;
            continue;
        }
        float Ux = 1.0f, Uy = 0.0f;         // along the body
        float Least = -1.0f;
        for (int c = 0; c < Count; c++)
        {
            if (Pieces[c].bRound || FlameRoot(Group, c) != r)
                continue;
            const float Size = WalkedSize(r, Pieces[c].Across[0], Pieces[c].Across[1]);
            if (Least < 0.0f || Size < Least * (1.0f - 1.0e-4f))
            {
                Least = Size;
                Ux    = Pieces[c].Across[0];
                Uy    = Pieces[c].Across[1];
            }
        }
        const float Vx = -Uy, Vy = Ux;      // across it
        float U0 = 1e9f, U1 = -1e9f, V0 = 1e9f, V1 = -1e9f, Bottom = 1e9f, Top = -1e9f;
        // The same box of the pieces' tongues: what the body is where it is too small for a standing fire's shape.
        float Tu0 = 1e9f, Tu1 = -1e9f, Tv0 = 1e9f, Tv1 = -1e9f, TongueFoot = 1e9f, TongueTop = -1e9f;
        float Bright = 0.0f, Deep = 0.0f;
        int   Members = 0, Sheets = 0;
        for (int m = 0; m < Count; m++)
        {
            if (FlameRoot(Group, m) != r)
                continue;
            const FFlamePiece& P = Pieces[m];
            Sheets += P.bRound ? 0 : 1;
            for (int e = -1; e <= 1; e += 2)
            {
                const float X = P.Mid[0] + P.Across[0] * P.Half * (float)e;
                const float Y = P.Mid[1] + P.Across[1] * P.Half * (float)e;
                const float u = X * Ux + Y * Uy, v = X * Vx + Y * Vy;
                U0 = Lesser(U0, u); U1 = Greater(U1, u);
                V0 = Lesser(V0, v); V1 = Greater(V1, v);
                const float Tx = P.Mid[0] + P.Across[0] * P.Tongue[0] * (float)e;
                const float Ty = P.Mid[1] + P.Across[1] * P.Tongue[0] * (float)e;
                const float tu = Tx * Ux + Ty * Uy, tv = Tx * Vx + Ty * Vy;
                Tu0 = Lesser(Tu0, tu); Tu1 = Greater(Tu1, tu);
                Tv0 = Lesser(Tv0, tv); Tv1 = Greater(Tv1, tv);
            }
            if (P.bRound)
            {
                // A card's piece reaches as far across as along.
                const float v = P.Mid[0] * Vx + P.Mid[1] * Vy;
                V0  = Lesser(V0, v - P.Half);       V1  = Greater(V1, v + P.Half);
                Tv0 = Lesser(Tv0, v - P.Tongue[0]); Tv1 = Greater(Tv1, v + P.Tongue[0]);
            }
            Bottom     = Lesser(Bottom, P.Bottom);
            Top        = Greater(Top, P.Top);
            TongueFoot = Lesser(TongueFoot, P.Tongue[1]);
            TongueTop  = Greater(TongueTop, P.Tongue[2]);
            Bright    += P.Bright;
            Deep       = Greater(Deep, P.Depth);
            Members++;
        }
        if (Greater(0.5f * (U1 - U0), 0.5f * (V1 - V0)) < FireTongueHalf)
        {
            // One tongue of flame, standing where its pieces do: a candle's.
            const float Um     = 0.5f * (Tu0 + Tu1), Vm = 0.5f * (Tv0 + Tv1);
            const float MidZ   = 0.5f * (TongueFoot + TongueTop);
            float       Half   = 0.5f * Greater(Tu1 - Tu0, Tv1 - Tv0);
            float       HalfUp = 0.5f * (TongueTop - TongueFoot);
            const float Gain   = FireTongueGain(Half);
            const float Dim    = FireTongueWiden(Half, HalfUp, UnitsAPixel * Greater(Deep, 1.0f));
            F[0]  = Um * Ux + Vm * Vx;
            F[1]  = Um * Uy + Vm * Vy;
            F[2]  = MidZ - HalfUp;
            F[3]  = MidZ + HalfUp;
            F[4]  = 1.0f;
            F[5]  = 0.0f;
            F[6]  = F[7] = Greater(Half, FireTongueHalf / FireTongueGainMost);
            F[11] = 0.0f;
            F[12] = (float)FlameShape_Lick;
            F[13] = Bright / (float)((Members > 1) ? Members : 1);
            F[14] = Gain * Dim * FireTongueCore;
            N++;
            continue;
        }
        const float Um = 0.5f * (U0 + U1), Vm = 0.5f * (V0 + V1);
        const float HalfU = Greater(0.5f * (U1 - U0), FireBodyMinHalf);
        const float HalfV = Greater(Greater(0.5f * (V1 - V0), 0.4f * HalfU), FireBodyMinHalf);
        F[0]  = Um * Ux + Vm * Vx;
        F[1]  = Um * Uy + Vm * Vy;
        F[2]  = Bottom;
        F[3]  = Top;
        F[4]  = Ux;
        F[5]  = Uy;
        F[6]  = HalfU;
        F[7]  = HalfV;
        // A seed from where it stands: the same body is the same flame every frame.
        F[11] = 0.0f;       // no seed of its own: the walk reads its grain by where the body stands
        F[12] = (float)FlameShape_Standing;
        F[13] = 1.0f;
        // As many times thinner its glow as the body is larger; its grain as many times coarser only where a level's
        // sheet is among its pieces. A body of the game's own flames alone - a burning floor's cards, a burning man's -
        // changes its box whenever the game lights a flame or puts one out, and a grain that went by the box jumped
        // with it: that one keeps the plain grain, which is the level's own and stays where it is.
        const float Larger = FireBodyScale(HalfU, HalfV);
        F[15] = (Sheets > 0) ? Larger : 1.0f;
        F[14] = 1.0f / Larger;
        N++;
    }
    return N;
}
// The edge of a pick. Where a few of many are taken by strength, the one taken last and the first one left out change
// places as their strengths cross - and what one of them shows would be there and gone at once. So each of those taken
// has a share of its own by how much stronger it is than the strongest left out (Cut; nothing left out: 0): none
// where they are level, all of it once it is Fade of its own strength ahead. The two are then both at nothing as they
// pass.
inline float PickEdgeShare(float Own, float Cut, float Fade)
{
    if (!(Cut > 0.0f))
        return 1.0f;
    const float Band = Fade * Own;
    if (!(Band > 0.0f))
        return (Own > Cut) ? 1.0f : 0.0f;
    const float Of = (Own - Cut) / Band;
    return (Of < 0.0f) ? 0.0f : ((Of > 1.0f) ? 1.0f : Of);
}
//   Light shafts  the beams a lamp throws through the air where a wall lets its light past. Walked along the
//               same ray as the smoke, asking the level's own shadow cubes at each step. Strength is per
//               hundred units of air; MaxWorld is how far down the ray they are gathered at all, because the
//               far end of a long street adds nothing an eye can see and costs the same as the near end.
//               The lamps: a shaft is asked of its lamp at every step of every pixel inside its reach, so what the
//               shafts cost goes by the lamps whose reach holds the view. They come from the strongest LampPlaces
//               of the shadows' lights that can show on the screen. Where there are more, each of those takes a
//               share of its light by how much stronger it is than the first one left out, all of it once it is
//               LampFade of its own strength ahead: two lamps changing places at the edge of the pick are both at
//               nothing as they pass, and no shaft comes or goes at once.
static const int   ShaftLampPlaces      = 8;
static const float ShaftLampFade        = 0.25f;
// Picks them: Score is each candidate's strength (none below zero), Count of them. Order gets the places of those
// taken, the strongest first, and Share each one's share of its light, 0..1; the answer is how many - Most at most.
inline int ShaftLampPick(const float* Score, int Count, int Most, int* Order, float* Share)
{
    int Taken = 0;
    float Cut = 0.0f;       // the strongest of those left out
    for (int i = 0; i < Count; i++)
    {
        int At = Taken;
        while (At > 0 && Score[Order[At - 1]] < Score[i])
            At--;
        if (At >= Most)
        {
            Cut = (Score[i] > Cut) ? Score[i] : Cut;
            continue;
        }
        if (Taken == Most)
        {
            Cut = (Score[Order[Most - 1]] > Cut) ? Score[Order[Most - 1]] : Cut;
            Taken--;
        }
        for (int k = Taken; k > At; k--)
            Order[k] = Order[k - 1];
        Order[At] = i;
        Taken++;
    }
    for (int k = 0; k < Taken; k++)
        Share[k] = PickEdgeShare(Score[Order[k]], Cut, ShaftLampFade);
    return Taken;
}
static const float ShaftStrengthDefault = 1.20f;
static const int   ShaftStepsDefault    = 24;
static const float ShaftMaxWorld        = 4000.0f;
static const float ShaftBiasWorld       = 3.0f;
// Much less forward than the smoke's 0.45. A beam is most often looked at FROM THE SIDE, and at that angle a
// strongly forward phase halves it - the one view the effect exists for would be its dimmest. Measured.
static const float ShaftForward         = 0.25f;

static const float CloudFadeOut    = 0.45f;
//   CloudKeepAway  how long a puff is kept after it leaves the VIEW. The engine culls a sprite whose card is
//               off the screen, so it simply stops arriving - and a cloud that loses its lower half because the
//               player looked up is not a cloud. A volume reaches past its card, so one just outside still
//               covers pixels at the edge, and one further out costs a step or two and nothing else. Dropped
//               after this, because a puff gone this long really is gone.
static const float CloudKeepAway   = 6.0f;
//   Puff        how thick one puff is. The game paints every card of smoke alike whatever its size - the wisp over an
//               incense stick as bright as a cloud of gas, the puff where a bullet struck as the steam out of a vent -
//               and the cards it places are from a unit and a half across to ninety. So a puff's thickness goes by
//               its size: the view through its middle closes up by CloudPuffDepth (an optical depth, before the noise
//               carves it), the same for a puff a unit across and one a hundred. The game's own fade of the card
//               thins it by as much: a card faded to a tenth is a tenth of the smoke, not the same smoke painted dark
//               - under the game's blend a faded card only adds less, it never darkens what is behind it.
//               CloudPuffPath: what a puff's profile, (1 - d * d) squared across its middle, comes to along a
//               diameter, in radii.
static const float CloudPuffDepth  = 0.60f;
static const float CloudPuffPath   = 16.0f / 15.0f;
inline float CloudPuffDensity(float Radius, float Fade, float Depth)
{
    const float R = (Radius > 0.25f) ? Radius : 0.25f;
    const float F = (Fade < 0.0f) ? 0.0f : ((Fade > 1.0f) ? 1.0f : Fade);
    return Depth * F / (CloudPuffPath * R);
}
//   MinPixels   a puff is never walked smaller on the target than this across (VolumeWiden): where a bullet struck,
//               a hundred paces off, the puff is less than a pixel of the walk's - widened, and thinned by as much.
static const float CloudPuffMinPixels = 3.0f;
//   GrainSize   the puff the noise's grain was shaped on, its radius. A smaller puff is carved by a finer grain - the
//               lumps of a wisp are a wisp's size - in whole halvings of the grain, at most CloudGrainLods of them,
//               the walk blending between two (Volumetrics.cpp, Thick): a grain that followed the size smoothly would
//               slide through the smoke as a puff grew.
static const float CloudGrainSize  = 24.0f;
static const int   CloudGrainLods  = 3;
//   Tile        the walk asks, at a pixel, only the puffs its ray can meet: they are listed by tile of the walk's own
//               target, CloudTile pixels a side (CloudPuffTiles).
static const int   CloudTile       = 32;
//   Reactive    the temporal upscaler's reactive mask where the walk lays smoke, which has no motion vectors: the share
//               of a pixel's cover - 0 with nothing in front of it, 1 hidden behind the smoke - that is told, 0 for
//               nothing told. Nothing is: a plume's movement gains nothing sure by it, and smoke can fill the view,
//               where a surface told this way all over shimmers (the see-through water, FBackend::Reactive).
static const float CloudReactive   = 0.0f;
//   TrackCell   the following of the puffs from frame to frame looks a puff's last place up in a grid of the level,
//               this many units a cell; TrackSpeed is the fastest a puff is taken to move, units a second - a puff is
//               looked for as far from its last place as that takes it in a frame, or three quarters of its own radius.
static const float CloudTrackCell  = 64.0f;
static const float CloudTrackSpeed = 400.0f;

// The tiles a ball can show in: Centre in view space (x and y as the screen's, z into the picture), Reach its radius.
// Focal and Screen are the engine's projection and its screen's centre, in its own pixels; EngPerPixel those pixels in
// one of the target's. The answer is a rectangle of tiles, both ends within it. False where the ball shows in none:
// behind the eye, or off the target. A ball that reaches the eye's own plane can show anywhere, and is given every
// tile. Never fewer tiles than the ball covers: the bound is its box's, not its outline's.
inline bool CloudPuffTiles(const float* Centre, float Reach, float Focal, const float* Screen, const float* EngPerPixel,
                           int TilesX, int TilesY, int& X0, int& Y0, int& X1, int& Y1)
{
    const float NearPlane = 1.0f;
    const float Near = Centre[2] - Reach;
    const float Far  = Centre[2] + Reach;
    if (!(Far > NearPlane) || TilesX <= 0 || TilesY <= 0 || !(EngPerPixel[0] > 0.0f) || !(EngPerPixel[1] > 0.0f))
        return false;
    if (!(Near > NearPlane))
    {
        X0 = 0;
        Y0 = 0;
        X1 = TilesX - 1;
        Y1 = TilesY - 1;
        return true;
    }
    float Lo[2], Hi[2];
    for (int k = 0; k < 2; k++)
    {
        // An edge on the far side of the middle shows furthest out at the ball's nearest depth, one that has not
        // reached the middle at its farthest.
        const float A = Centre[k] - Reach;
        const float B = Centre[k] + Reach;
        Lo[k] = (Screen[k] + Focal * ((A < 0.0f) ? A / Near : A / Far)) / EngPerPixel[k];
        Hi[k] = (Screen[k] + Focal * ((B > 0.0f) ? B / Near : B / Far)) / EngPerPixel[k];
    }
    const float Wide = (float)(TilesX * CloudTile);
    const float Tall = (float)(TilesY * CloudTile);
    if (Hi[0] < 0.0f || Hi[1] < 0.0f || Lo[0] >= Wide || Lo[1] >= Tall)
        return false;
    X0 = (Lo[0] > 0.0f) ? (int)(Lo[0] / (float)CloudTile) : 0;
    Y0 = (Lo[1] > 0.0f) ? (int)(Lo[1] / (float)CloudTile) : 0;
    X1 = (Hi[0] < Wide) ? (int)(Hi[0] / (float)CloudTile) : TilesX - 1;
    Y1 = (Hi[1] < Tall) ? (int)(Hi[1] / (float)CloudTile) : TilesY - 1;
    if (X1 > TilesX - 1) X1 = TilesX - 1;
    if (Y1 > TilesY - 1) Y1 = TilesY - 1;
    return true;
}

// The tiles a side of the walk's target is cut into: the last may reach past the target's edge.
inline int CloudTilesAlong(int Side)
{
    return (Side > 0) ? (Side + CloudTile - 1) / CloudTile : 0;
}

// The numbers the tile lists have room for from the start, for a target of that size: every tile's own two (below),
// whatever a frame's puffs, and never fewer than Least. The walk reads a pixel's tile's two wherever its lists are
// bound, so the room is there before the first puff is.
inline int CloudTileFirstRoom(int TargetW, int TargetH, int Least)
{
    const int Heads = 2 * CloudTilesAlong(TargetW) * CloudTilesAlong(TargetH);
    return (Heads > Least) ? Heads : Least;
}

// The puffs' lists by tile, from each puff's rectangle of tiles (CloudPuffTiles: X0 Y0 X1 Y1, four numbers a puff; X1
// below X0 for a puff that shows in none). The numbers are, for every tile, where its list starts and how many it
// holds - two a tile, the tiles row by row - and then the lists one after another, each a puff's place among the puffs.
// CloudTileListsSize: how many numbers that is. CloudTileListsFill writes them into Out, which has room for as many,
// and gives the length of the longest list.
inline int CloudTileListsSize(const int* Rects, int Count, int TilesX, int TilesY)
{
    int Total = TilesX * TilesY * 2;
    for (int i = 0; i < Count; i++)
    {
        const int* const R = Rects + i * 4;
        if (R[2] >= R[0] && R[3] >= R[1])
            Total += (R[2] - R[0] + 1) * (R[3] - R[1] + 1);
    }
    return Total;
}
inline int CloudTileListsFill(const int* Rects, int Count, int TilesX, int TilesY, unsigned int* Out)
{
    const int Tiles = TilesX * TilesY;
    for (int t = 0; t < Tiles * 2; t++)
        Out[t] = 0u;
    // Counted first, then filled, so the lists lie one after another.
    for (int i = 0; i < Count; i++)
    {
        const int* const R = Rects + i * 4;
        for (int y = R[1]; y <= R[3] && R[2] >= R[0]; y++)
            for (int x = R[0]; x <= R[2]; x++)
                Out[(y * TilesX + x) * 2 + 1]++;
    }
    unsigned int At   = (unsigned int)(Tiles * 2);
    int          Most = 0;
    for (int t = 0; t < Tiles; t++)
    {
        const unsigned int Held = Out[t * 2 + 1];
        Most           = ((int)Held > Most) ? (int)Held : Most;
        Out[t * 2]     = At;
        Out[t * 2 + 1] = 0u;        // counted up again as the list is filled
        At += Held;
    }
    for (int i = 0; i < Count; i++)
    {
        const int* const R = Rects + i * 4;
        for (int y = R[1]; y <= R[3] && R[2] >= R[0]; y++)
            for (int x = R[0]; x <= R[2]; x++)
            {
                unsigned int* const T = Out + (y * TilesX + x) * 2;
                Out[T[0] + T[1]++] = (unsigned int)i;
            }
    }
    return Most;
}

// The ball a flame body's light stays inside, in the world: what the tiles it is listed in are worked out from
// (FlameBodyTiles). Body: its FlameBodyFloats numbers. Mid gets the ball's middle; the answer is its radius. It holds
// all the walk can show of the body (Volumetrics.cpp): the upright cylinder a ray's stretch through it is cut from
// (FireStretches) and the shape its heat is read in (FireAt) - a fireball's reaches past its cylinder - and, with
// bSparks, a standing fire's sparks (Sparks), which fly far from it. The walk asks a pixel only the bodies of its
// tile: a ball smaller than what the walk shows of a body cuts its flame off along the tiles' edges.
inline float FlameBodyBall(const float* Body, bool bSparks, float* Mid)
{
    auto Lesser  = [](float A, float B) -> float { return (A < B) ? A : B; };
    auto Greater = [](float A, float B) -> float { return (A > B) ? A : B; };
    const float Tall    = Greater(Body[3] - Body[2], 1.0f);
    const bool  bStands = Body[12] < 0.5f;
    if (Body[12] > 2.5f)
    {
        // A body of glow is gone at its own surface - the walk's ball of glow, drawn out to the body's length,
        // width and height: the ball round its middle as far out as its longest half, which the walk cuts the
        // body's stretch of a ray to (FireStretches) - and a hair more, for the walk's own rounding. No further: a
        // burst of sparks is a hundred of them one over another, and every tile a body is listed in asks it at
        // every step.
        Mid[0] = Body[0];
        Mid[1] = Body[1];
        Mid[2] = 0.5f * (Body[2] + Body[3]);
        return 1.02f * Greater(Greater(Body[6], Body[7]), 0.5f * (Body[3] - Body[2])) + 0.05f;
    }
    // How far out from its middle on the level, and from how low to how high.
    float Out = Greater(Body[6], Body[7]);
    float Low, High;
    if (bStands)
    {
        Low  = Body[2] - Greater(2.0f, 0.1f * Tall);
        High = Body[2] + FireWalkReach * Tall;
    }
    else
    {
        // The stretch's cylinder: from two units under the foot to a quarter of its height and two units over the top.
        Low  = Body[2] - 2.0f;
        High = Body[3] + 0.25f * (Body[3] - Body[2]) + 2.0f;
        if (Body[12] < 1.5f)
        {
            const float Round = FireBallOut * Body[6];
            const float Cz    = 0.5f * (Body[2] + Body[3]);
            Out  = Greater(Out, Round);
            Low  = Lesser(Low, Cz - FireBallTall * Round);
            High = Greater(High, Cz + FireBallTall * Round);
        }
        else
        {
            // A lick's shape: from a tenth of its height under its foot to a tenth over its top.
            Low  = Lesser(Low, Body[2] - 0.1f * Tall);
            High = Greater(High, Body[2] + 1.1f * Tall);
        }
    }
    Mid[0] = Body[0];
    Mid[1] = Body[1];
    Mid[2] = 0.5f * (Low + High);
    const float Up    = 0.5f * (High - Low);
    float       Reach = Sqrt(Out * Out + Up * Up);
    if (bSparks && bStands)
    {
        // The sparks stay inside a ball round the flame's top: one ball round that and the body, from this middle.
        const float Size  = Lesser(Greater(Tall / 32.0f, 0.5f), 3.0f);
        const float Fly   = Greater(Body[6], Body[7]) + Tall + FireSparkFly * Size;
        const float Above = Body[3] - Mid[2];
        Reach = Greater(Reach, ((Above < 0.0f) ? -Above : Above) + Fly);
    }
    return Reach;
}

// The tiles of the walk's target a flame body can show in: its ball's (FlameBodyBall, CloudPuffTiles). Eye and the
// three axes are the view's in the world - a point of the world is, to the view, its way from the eye along each.
// False where it shows in none.
inline bool FlameBodyTiles(const float* Body, bool bSparks, const float* Eye, const float* AxisX, const float* AxisY,
                           const float* AxisZ, float Focal, const float* Screen, const float* EngPerPixel, int TilesX,
                           int TilesY, int& X0, int& Y0, int& X1, int& Y1)
{
    float       Mid[3];
    const float Reach   = FlameBodyBall(Body, bSparks, Mid);
    const float D[3]    = { Mid[0] - Eye[0], Mid[1] - Eye[1], Mid[2] - Eye[2] };
    const float View[3] = { D[0] * AxisX[0] + D[1] * AxisX[1] + D[2] * AxisX[2],
                            D[0] * AxisY[0] + D[1] * AxisY[1] + D[2] * AxisY[2],
                            D[0] * AxisZ[0] + D[1] * AxisZ[1] + D[2] * AxisZ[2] };
    return CloudPuffTiles(View, Reach, Focal, Screen, EngPerPixel, TilesX, TilesY, X0, Y0, X1, Y1);
}

// Which of the game's force fields a texture is (ForceField), 0 for none of them: the three of the BobPage package -
// 1 for BP_FX_01, the large level sheets'; 2 for BP_FX_02, the one the cabinets of the augmentation canisters are
// closed with; 3 for BP_FX_03, the cells' and the doorways' -, wherever the levels use them. The whole name,
// whatever its letters' case.
template <typename CH>
inline int FieldKind(const CH* PathName)
{
    if (!PathName)
        return 0;
    static const char Want[] = "bobpage.bp_fx_0";
    int n = 0;
    for (; Want[n] != 0; n++)
    {
        CH Ch = PathName[n];
        if (Ch >= (CH)'A' && Ch <= (CH)'Z')
            Ch = (CH)(Ch - (CH)'A' + (CH)'a');
        if (Ch != (CH)Want[n])
            return 0;
    }
    return (PathName[n] >= (CH)'1' && PathName[n] <= (CH)'3' && PathName[n + 1] == 0) ? (int)(PathName[n] - (CH)'0') : 0;
}
template <typename CH>
inline bool IsFieldTexture(const CH* PathName)
{
    return FieldKind(PathName) != 0;
}

// The name arrives as the engine spells it: Package.Group.Name. A template over the character type, as everything
// here that touches text is - this header takes no engine include, so it serves the game's wide names and the tests'
// narrow ones alike, and the word is spelled out rather than wrapped in TEXT().
template <typename CH>
inline bool IsCloudTexture(const CH* PathName)
{
    if (!PathName)
        return false;
    static const char Want[] = "effects.smoke.";
    for (const CH* P = PathName; *P; P++)
    {
        int  n     = 0;
        bool bSame = true;
        while (Want[n] != 0)
        {
            // Reading past the end of the name cannot happen: its terminating zero matches nothing in the word, so
            // the comparison stops there.
            CH Ch = P[n];
            if (Ch >= (CH)'A' && Ch <= (CH)'Z')
                Ch = (CH)(Ch - (CH)'A' + (CH)'a');
            if (Ch != (CH)Want[n])
            {
                bSame = false;
                break;
            }
            n++;
        }
        if (bSame)
            return true;
    }
    return false;
}

enum EWater
{
    Water_Off = 0,
    // The water made real: seen through, bending the light, its surface lifted by its waves, with rings and splashes.
    Water_Low,
    // Low, with the caustics: the net of light the waves throw, on whatever lies under the water (WaterHasCaustics).
    Water_High,
    Water_Count
};

inline const char* const* WaterNames()
{
    static const char* const Names[Water_Count] = { "Off", "Low", "High" };
    return Names;
}

// What a level of the water has besides the see-through surface: the caustics at High. What its surface mirrors is
// the reflections' to say (EReflections).
inline bool WaterHasCaustics(int Level) { return Level >= (int)Water_High; }

// One water of several zones. The engine cuts a flooded place into zones at its doorways - a room, the corridor
// beyond it - and those zones touch under the water with no border of a liquid between them. What goes by "which
// liquid a place is in" (the caustics: FBackend::LayCaustics) goes by the water: the zones that touch, one through
// another, named by the least of them. Body holds a number a zone: each its own to begin with (WaterBodiesBegin),
// two that touch joined (WaterBodiesJoin); WaterBodyOf says a zone's water.
inline void WaterBodiesBegin(int* Body, int Count)
{
    for (int z = 0; z < Count; z++)
        Body[z] = z;
}
inline int WaterBodyOf(const int* Body, int Count, int Zone)
{
    // A zone names a lesser one of its water, or itself: the chain ends at the least.
    for (int n = 0; n < Count && Zone >= 0 && Zone < Count && Body[Zone] != Zone; n++)
        Zone = Body[Zone];
    return Zone;
}
inline void WaterBodiesJoin(int* Body, int Count, int A, int B)
{
    if (A < 0 || A >= Count || B < 0 || B >= Count)
        return;
    const int LeastA = WaterBodyOf(Body, Count, A), LeastB = WaterBodyOf(Body, Count, B);
    if (LeastA < LeastB)
        Body[LeastB] = LeastA;
    else if (LeastB < LeastA)
        Body[LeastA] = LeastB;
}

// A mirror of several surfaces. The engine draws a mirror's view once for each of the level's surfaces the mirror is
// made of: the whole picture again, through another piece of the mirror - and hands over whole polygons, which reach
// over the other pieces' parts of the screen. Such views are one view going on, drawn by one depth: they look from
// the same place along the same axes, with the same projection onto the same part of the screen, and are cut along
// the same plane. View: a view's seventeen numbers - its place, its three axes, its projection, its screen's centre
// and its picture's far corner; Plane: the plane it is cut along, in its own space.
inline bool MirrorViewsAlike(const float* ViewA, const float* PlaneA, const float* ViewB, const float* PlaneB)
{
    const auto Near = [](float A, float B, float Within) { return A - B <= Within && B - A <= Within; };
    for (int i = 0; i < 17; i++)
        if (!Near(ViewA[i], ViewB[i], (i < 3) ? 0.05f : (i < 12) ? 1.0e-4f : 0.01f))
            return false;
    for (int i = 0; i < 3; i++)
        if (!Near(PlaneA[i], PlaneB[i], 1.0e-4f))
            return false;
    return Near(PlaneA[3], PlaneB[3], 0.05f);
}

// Which side of its mirror's plane a polygon of a mirror's view is kept on. The plane is given in the view's space,
// the eye at its origin, a point's distance from it its dot with the plane's normal less PlaneW. The side is the one
// the engine's own piece of the polygon lies on - PieceSum, the sum of that piece's corners' distances - and where the
// piece lies in the plane itself (the sliver the engine's cut leaves of a polygon that only touches the mirror), the
// side the eye is not on: a mirror's view looks from behind its mirror at what lies beyond it. 1 or -1; 0 where
// neither tells.
inline float MirrorKeptSide(float PieceSum, int PieceCorners, float PlaneW)
{
    const float Small = 0.01f * (float)((PieceCorners > 0) ? PieceCorners : 1);
    if (PieceSum > Small)
        return 1.0f;
    if (PieceSum < -Small)
        return -1.0f;
    return (PlaneW > 0.01f) ? 1.0f : (PlaneW < -0.01f) ? -1.0f : 0.0f;
}

// The caustics are laid on what lies in a see-through liquid, under its level surface (FBackend::LayCaustics): the
// surfaces of at most CausticBoxesMost waters, those within CausticReach of the camera, each taken CausticBoxMargin
// wider than its own extent - a pool's walls stand on its very edge. CausticBoxFloats numbers a surface: its lowest X
// and Y, its highest X and Y, its height, and its water (WaterBodyOf its liquid's zone) plus one.
// What is IN the liquid is told by the liquid's borders with the zones around it - the surface itself, a pane between
// a tank and a room: a mask of the picture drawn from them, at most CausticPortalVertsMost corners of their triangles a
// frame, CausticPortalFloats numbers each - the corner's place on the level and what its border writes
// (CausticMaskValue).
enum { CausticBoxesMost = 4, CausticBoxFloats = 6, CausticPortalVertsMost = 12288, CausticPortalFloats = 4 };
static const float CausticReach     = 4096.0f;
static const float CausticBoxMargin = 8.0f;

// What the caustics' mask says of a pixel, in 255ths: 0 - no border of a liquid lies in front of it, so it is in what
// the camera is in; 1 - the last border before it leads out of a liquid; from 2 - into the water whose number
// (WaterBodyOf) is that less two. The shader takes one off, to the water plus one that a surface's numbers carry
// (0: in no liquid).
inline float CausticMaskValue(bool bInto, int Zone)
{
    return (float)(bInto ? Zone + 2 : 1) / 255.0f;
}

template <typename TChar>
inline void WaterName(EWater W, TChar* Out, int Max)
{
    const char* const N = WaterNames()[(W >= 0 && W < Water_Count) ? (int)W : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Off for anything it does not recognise, with bKnown false so the caller can say so in the log. Medium and RT are taken
// as well, which a settings file says where the water's level stands for its mirror image too: Medium is Low and RT is
// High, and what they say of the mirror image the reflections take (ReflectionsOfOldWater).
template <typename TChar>
inline EWater ParseWater(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return Water_Off;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    int E = B;
    while (Text[E])
        E++;
    while (E > B && (Text[E - 1] == ' ' || Text[E - 1] == '\t'))
        E--;
    const int Len = E - B;
    const char* const Spellings[Water_Count + 2] = { "Off", "Low", "High", "Medium", "RT" };
    static const EWater Means[Water_Count + 2]   = { Water_Off, Water_Low, Water_High, Water_Low, Water_High };
    for (int q = 0; q < (int)(Water_Count + 2); q++)
    {
        const char* N = Spellings[q];
        int k = 0;
        for (; k < Len && N[k]; k++)
        {
            unsigned C = (unsigned)Text[B + k];
            if (C >= 'A' && C <= 'Z')
                C += 'a' - 'A';
            unsigned D = (unsigned)N[k];
            if (D >= 'A' && D <= 'Z')
                D += 'a' - 'A';
            if (C != D)
                break;
        }
        if (k == Len && N[k] == 0)
        {
            bKnown = true;
            return Means[q];
        }
    }
    return Water_Off;
}

// Reflections: what gives a mirror image back - the water's surface, and the levels' glass (IsGlassSurface).
// Default: none of ours - the water without a mirror image, the glass as the level draws it. High: from the finished
// picture - what is on the screen can be mirrored, and the sky; on any card. RT: from rays traced through the level -
// what is off the screen or behind the viewer too; on a card that traces rays, with the shadows on, whose scene the
// rays are traced against - elsewhere High runs in its place. RTHalf: RT with a quarter of the rays - of each two by
// two pixels one traces for the others where their rays are alike (SolidPass.hlsl, RtAskLed); in the menu's order
// between High and RT.
enum EReflections
{
    Reflections_Default = 0,
    Reflections_High,
    Reflections_RtHalf,
    Reflections_Rt,
    Reflections_Count
};

inline const char* const* ReflectionsNames()
{
    static const char* const Names[Reflections_Count] = { "Default", "High", "RTHalf", "RT" };
    return Names;
}

inline bool ReflectionsHasMirror(int Level) { return Level >= (int)Reflections_High; }
// Whether the mirror image comes from rays: RTHalf and RT.
inline bool ReflectionsTraced(int Level) { return Level >= (int)Reflections_RtHalf; }

template <typename TChar>
inline void ReflectionsName(EReflections R, TChar* Out, int Max)
{
    const char* const N = ReflectionsNames()[(R >= 0 && R < Reflections_Count) ? (int)R : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}

// Whether a setting's text is a word, spaces around it and the case of its letters aside.
template <typename TChar>
inline bool SettingIsWord(const TChar* Text, const char* Word)
{
    if (!Text || !Word)
        return false;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    int E = B;
    while (Text[E])
        E++;
    while (E > B && (Text[E - 1] == ' ' || Text[E - 1] == '\t'))
        E--;
    int k = 0;
    for (; k < E - B && Word[k]; k++)
    {
        unsigned C = (unsigned)Text[B + k];
        if (C >= 'A' && C <= 'Z')
            C += 'a' - 'A';
        unsigned W = (unsigned)Word[k];
        if (W >= 'A' && W <= 'Z')
            W += 'a' - 'A';
        if (C != W)
            return false;
    }
    return k == E - B && Word[k] == 0;
}

// Default for anything it does not recognise, with bKnown false so the caller can say so in the log. "Off" is Default.
template <typename TChar>
inline EReflections ParseReflections(const TChar* Text, bool& bKnown)
{
    static const char* const Spellings[Reflections_Count + 1] = { "Default", "High", "RTHalf", "RT", "Off" };
    static const EReflections Means[Reflections_Count + 1]    = { Reflections_Default, Reflections_High,
                                                                  Reflections_RtHalf, Reflections_Rt,
                                                                  Reflections_Default };
    bKnown = false;
    for (int q = 0; q < (int)(Reflections_Count + 1); q++)
        if (SettingIsWord(Text, Spellings[q]))
        {
            bKnown = true;
            return Means[q];
        }
    return Reflections_Default;
}

// The menu's step: Default, High, RTHalf, RT, round again - Step places along (-1 or 1).
inline EReflections ReflectionsStep(EReflections R, int Step)
{
    int N = ((int)R + Step) % (int)Reflections_Count;
    if (N < 0)
        N += Reflections_Count;
    return (EReflections)N;
}

// The levels' glass: a surface the level draws see-through with the game's window glass - the texture WindOpacStrek
// in any of its variants (_A to _E, 07WindOpacStrek, the copy in the NycBar package): what its shop windows, its
// shelters' walls, its glazed doors and its glass table tops wear. By the texture's own name - the last part of its
// full name - and by that one texture alone: the other textures called glass are frosted panes, stained glass,
// screens and lamps. Where the level draws it solid it is no pane. Never a surface the engine itself mirrors
// (PF_Mirrored): that one shows the engine's own mirror image, and is left as it is.
const unsigned int RDX_PF_Mirrored = 0x08000000u;

template <typename TChar>
inline bool GlassTextureName(const TChar* Name)
{
    static const char* const Word = "windopacstrek";
    if (!Name)
        return false;
    for (int At = 0; Name[At]; At++)
    {
        int k = 0;
        for (; Word[k] && Name[At + k]; k++)
        {
            unsigned C = (unsigned)Name[At + k];
            if (C >= 'A' && C <= 'Z')
                C += 'a' - 'A';
            if (C != (unsigned)Word[k])
                break;
        }
        if (!Word[k])
            return true;
    }
    return false;
}

inline bool IsGlassSurface(unsigned int PolyFlags, bool bGlassName)
{
    return bGlassName && BlendFor(PolyFlags) == Blend_Translucent && !(PolyFlags & RDX_PF_Mirrored);
}

// A pane's normal, on the camera's side, from its polygon's corners in view space - where the camera is the origin.
// The widest of the fan's triangles about the first corner says it: the engine hands a polygon over cut to the view,
// and a cut can leave three corners in a row all but on one line. False for a polygon with no area.
inline bool PaneNormalToEye(const FVec3* Points, int Count, FVec3& Out)
{
    float Best = 0.0f;
    for (int i = 1; i + 1 < Count; i++)
    {
        const float Ax = Points[i].X - Points[0].X, Ay = Points[i].Y - Points[0].Y, Az = Points[i].Z - Points[0].Z;
        const float Bx = Points[i + 1].X - Points[0].X, By = Points[i + 1].Y - Points[0].Y;
        const float Bz = Points[i + 1].Z - Points[0].Z;
        const float Nx = Ay * Bz - Az * By, Ny = Az * Bx - Ax * Bz, Nz = Ax * By - Ay * Bx;
        const float Sq = Nx * Nx + Ny * Ny + Nz * Nz;
        if (Sq > Best)
        {
            Best  = Sq;
            Out.X = Nx;
            Out.Y = Ny;
            Out.Z = Nz;
        }
    }
    if (Best <= 1.0e-8f)
        return false;
    const float Inv = ((Out.X * Points[0].X + Out.Y * Points[0].Y + Out.Z * Points[0].Z > 0.0f) ? -1.0f : 1.0f) /
                      Sqrt(Best);
    Out.X *= Inv;
    Out.Y *= Inv;
    Out.Z *= Inv;
    return true;
}

// The reflections a settings file means that has no key for them: there the water's level stands for its mirror
// image too - Medium and High say it comes from the picture, RT from rays, anything else that there is none - and
// read so, the picture such a file gives stays what it is.
template <typename TChar>
inline EReflections ReflectionsOfOldWater(const TChar* WaterText)
{
    if (SettingIsWord(WaterText, "RT"))
        return Reflections_Rt;
    if (SettingIsWord(WaterText, "Medium") || SettingIsWord(WaterText, "High"))
        return Reflections_High;
    return Reflections_Default;
}

// Reflections=RT's colours for what a ray meets where the picture cannot show it (SolidPass.hlsl, WaterReflectRt). A
// texture's average colour: three bytes as the pipeline's colour space has them (RtAlbedoByte), and RtAlbedoOwnLight
// when the surface carries its own light (PF_Unlit, an unlit actor). A light: ten bits a channel over 0..RtLightRange -
// the light maps' doubled range, with room above it for the gain - or RtLightUnknown, none seen yet.
static const unsigned int RtAlbedoOwnLight = 1u << 24;
// A level surface's record for the mirror rays, in numbers (RDX_RT_RECORD_BYTES), and where its parts are.
enum { RtRecordUints = RDX_RT_RECORD_BYTES / 4, RtRecordTexture = 2, RtRecordLightMap = 3, RtRecordRows = 4,
       RtRecordDensity = 12, RtRecordGain = RDX_RT_RECORD_GAIN / 4, RtRecordLightRows = RDX_RT_RECORD_LIGHT_ROWS / 4,
       RtRecordLightEdge = RDX_RT_RECORD_LIGHT_EDGE / 4, RtRecordNormal = RDX_RT_RECORD_NORMAL / 4,
       RtRecordAmbient = RDX_RT_RECORD_AMBIENT / 4, RtRecordKind = RDX_RT_RECORD_KIND / 4 };
// What a surface or a caster is marked with beside its colour: lit only by the lamps marked the same.
static const unsigned int RtAlbedoSpecialLit = 1u << 25;
// A caster the engine has shown: its row holds the engine's own light on it, by the world's six ways.
static const unsigned int RtAlbedoEngineLit  = 1u << 26;
// The player's own body while first person does not draw it: the picture never shows it, so nothing of it is taken
// from the picture.
static const unsigned int RtAlbedoOwnBody    = 1u << 27;
// The record's two rows: where on its texture a place of the level lies, in repeats of the texture - U = Row[0..2] .
// place + Row[3], V = Row[4..6] . place + Row[7]. From the surface's mapping axes and origin in the world - the
// axes' lengths carry the texture's scale, as SurfaceCoord has it - its pan, and one over its size times its scale
// each way: what the surface's own vertices get, said for any place.
inline void RtSurfaceRows(const FVec3& AxisU, const FVec3& AxisV, const FVec3& Origin, float PanU, float PanV,
                          float InvU, float InvV, float* Row)
{
    Row[0] = AxisU.X * InvU;
    Row[1] = AxisU.Y * InvU;
    Row[2] = AxisU.Z * InvU;
    Row[3] = (-Dot(AxisU, Origin) - PanU) * InvU;
    Row[4] = AxisV.X * InvV;
    Row[5] = AxisV.Y * InvV;
    Row[6] = AxisV.Z * InvV;
    Row[7] = (-Dot(AxisV, Origin) - PanV) * InvV;
}
// How many of the texture's texels a unit of the level holds, along the denser of its two ways: what a mirror ray
// picks the texture's level of detail by.
inline float RtSurfaceDensity(const float* Row, int USize, int VSize)
{
    const float U = Sqrt(Row[0] * Row[0] + Row[1] * Row[1] + Row[2] * Row[2]) * (float)USize;
    const float V = Sqrt(Row[4] * Row[4] + Row[5] * Row[5] + Row[6] * Row[6]) * (float)VSize;
    return (U > V) ? U : V;
}

// The far probe (FRtPass::UpdateFar) takes what the picture shows this many units off or further: the sky, a skyline at the
// level's edge - never a near wall, which would stand in every direction behind it. The same as the mirror image's reach
// through the picture (SolidPass.hlsl, WaterReflect). RenderLimits.h's, which the water's shader reads too.
static const float        RtFarMin         = RDX_RT_FAR_MIN;
static const unsigned int RtLightUnknown   = 0xFFFFFFFFu;
static const float        RtLightRange     = 4.0f;

// A colour byte as the geometry pass works with it: as it is in the classic pipeline, decoded from sRGB in the linear
// one - a cubic close to the curve (within 0.3% of it), for an average colour that needs no more.
inline unsigned char RtAlbedoByte(unsigned char V)
{
#if RDX_CLASSIC
    return V;
#else
    const float C = V / 255.0f;
    const float L = C * (C * (C * 0.305306011f + 0.682171111f) + 0.012522878f);
    const float B = L * 255.0f + 0.5f;
    return (unsigned char)(B < 0.0f ? 0.0f : (B > 255.0f ? 255.0f : B));
#endif
}

inline unsigned int PackRtAlbedo(unsigned char R, unsigned char G, unsigned char B, bool bOwnLight)
{
    return (unsigned int)R | ((unsigned int)G << 8) | ((unsigned int)B << 16) | (bOwnLight ? RtAlbedoOwnLight : 0u);
}

inline unsigned int PackRtLight(float R, float G, float B)
{
    const float C[3] = { R, G, B };
    unsigned int Out = 0;
    for (int k = 0; k < 3; k++)
    {
        float V = C[k] / RtLightRange;
        V = (V < 0.0f) ? 0.0f : ((V > 1.0f) ? 1.0f : V);
        Out |= ((unsigned int)(V * 1023.0f + 0.5f)) << (10 * k);
    }
    return Out;
}

// What the shader reads back out of PackRtLight: channel k, 0 red.
inline float UnpackRtLight(unsigned int V, int k)
{
    return (float)((V >> (10 * k)) & 1023u) * (RtLightRange / 1023.0f);
}

// A light map's average light (RGBA7, as the engine hands it over), as the geometry pass lights the surface with it: every
// texel's bytes doubled and saturated (Rgba7Channel), red and blue the other way round (LightMapTexel), then times the
// surface's gain - the vertex colour its draws carry. Packed (PackRtLight); RtLightUnknown for no texels.
inline unsigned int RtLightAverage(const unsigned char* Texels, int Count, float Gain)
{
    if (!Texels || Count <= 0)
        return RtLightUnknown;
    unsigned int Sum[3] = { 0, 0, 0 };
    for (int i = 0; i < Count; i++, Texels += 4)
    {
        Sum[0] += Rgba7Channel(Texels[2]);
        Sum[1] += Rgba7Channel(Texels[1]);
        Sum[2] += Rgba7Channel(Texels[0]);
    }
    const float S = Gain / (255.0f * (float)Count);
    return PackRtLight((float)Sum[0] * S, (float)Sum[1] * S, (float)Sum[2] * S);
}

// A caster's vertices for the mirror rays, in the three floats the shadow maps' masks read - u, v and the mask's slot
// (FShadowMaps::CasterUvw) - with no fourth: the texture its face wears, whether the face carries its own light, and
// the vertex's normal ride in the same numbers, each too small to move what the masks read.
//  - In the slot's fraction, in 64ths: twice the face's key plus one - which of the caster's textures it wears
//    (RtCasterKeys of them at most, as its row of the rays' table lists them - SolidPass.hlsl, RtGeoTexture; none:
//    0) - and one more for a face with its own light. Always under a half: the slot still rounds to itself, and
//    the solid slot stays below a half, which is all the masks' readers ask.
//  - In u's and v's fractions of a 4096th: the normal, as two steps of 0..RtNormalSteps-1 on an octahedron
//    unfolded to a square. A mesh's u and v are bytes over 256, so the fraction is free; it moves a mask's lookup
//    by a sixteenth of a texel at the most.
enum { RtCasterKeys = 15, RtNormalSteps = RDX_RT_NORMAL_STEPS };
inline float PackCasterSlot(int Slot, int Key, bool bOwnLight)
{
    const int K = (Key >= 0 && Key < RtCasterKeys) ? Key + 1 : 0;
    return (float)Slot + (float)(K * 2 + (bOwnLight ? 1 : 0)) / 64.0f;
}
inline int CasterSlotOf(float W)
{
    return (int)(W + 0.5f);
}
inline int CasterKeyOf(float W)
{
    return ((int)((W - (float)CasterSlotOf(W)) * 64.0f + 0.5f) >> 1) - 1;
}
inline bool CasterOwnLightOf(float W)
{
    return ((int)((W - (float)CasterSlotOf(W)) * 64.0f + 0.5f) & 1) != 0;
}
inline void PackOctNormal(const FVec3& N, int& OctX, int& OctY)
{
    const float AX = (N.X < 0.0f) ? -N.X : N.X, AY = (N.Y < 0.0f) ? -N.Y : N.Y, AZ = (N.Z < 0.0f) ? -N.Z : N.Z;
    const float L  = AX + AY + AZ;
    float X = (L > 1e-12f) ? N.X / L : 0.0f;
    float Y = (L > 1e-12f) ? N.Y / L : 0.0f;
    if (N.Z < 0.0f)
    {
        // The lower half folds outwards: each corner of the square is straight down.
        const float FX = (1.0f - ((Y < 0.0f) ? -Y : Y)) * ((X >= 0.0f) ? 1.0f : -1.0f);
        const float FY = (1.0f - ((X < 0.0f) ? -X : X)) * ((Y >= 0.0f) ? 1.0f : -1.0f);
        X = FX;
        Y = FY;
    }
    const float S = (float)(RtNormalSteps - 1);
    OctX = (int)((X * 0.5f + 0.5f) * S + 0.5f);
    OctY = (int)((Y * 0.5f + 0.5f) * S + 0.5f);
}
inline FVec3 UnpackOctNormal(int OctX, int OctY)
{
    const float S = (float)(RtNormalSteps - 1);
    float X = (float)OctX / S * 2.0f - 1.0f;
    float Y = (float)OctY / S * 2.0f - 1.0f;
    const float Z = 1.0f - ((X < 0.0f) ? -X : X) - ((Y < 0.0f) ? -Y : Y);
    if (Z < 0.0f)
    {
        const float FX = (1.0f - ((Y < 0.0f) ? -Y : Y)) * ((X >= 0.0f) ? 1.0f : -1.0f);
        const float FY = (1.0f - ((X < 0.0f) ? -X : X)) * ((Y >= 0.0f) ? 1.0f : -1.0f);
        X = FX;
        Y = FY;
    }
    const float L = Sqrt(X * X + Y * Y + Z * Z);
    FVec3 Out;
    Out.X = (L > 1e-12f) ? X / L : 0.0f;
    Out.Y = (L > 1e-12f) ? Y / L : 0.0f;
    Out.Z = (L > 1e-12f) ? Z / L : 1.0f;
    return Out;
}
// u or v with one of the normal's two steps in its fraction, and the step read back.
inline float PackCasterPlace(float Place, int Oct)
{
    return Place + (float)Oct / (4096.0f * (float)RtNormalSteps);
}
inline int CasterOctOf(float Place)
{
    const float F = Place * 4096.0f;
    return (int)((F - (float)(int)F) * (float)RtNormalSteps + 0.5f);
}

// A caster's light for the mirror rays: what reaches it along each of the world's six ways - +x, -x, +y, -y, +z, -z,
// three numbers each (Cube, RtCubeFloats of them) - so that a face is lit by where it looks: each way's light by
// the square of its normal's part along it (RtCubeLight, and SolidPass.hlsl's own). A light from all round goes to
// the six alike; a lamp's to the ways that look towards it (To: the unit direction from the caster to the lamp),
// by how squarely each does. Made on the CPU a caster, from every lamp that reaches it - however many they are,
// the rays read six colours.
enum { RtCubeWays = 6, RtCubeFloats = RtCubeWays * 3 };
inline void RtCubeAddAll(float* Cube, float R, float G, float B)
{
    for (int w = 0; w < RtCubeWays; w++)
    {
        Cube[w * 3 + 0] += R;
        Cube[w * 3 + 1] += G;
        Cube[w * 3 + 2] += B;
    }
}
inline void RtCubeAddLamp(float* Cube, const FVec3& To, float R, float G, float B)
{
    const float D[3] = { To.X, To.Y, To.Z };
    for (int a = 0; a < 3; a++)
    {
        const int   w = a * 2 + ((D[a] >= 0.0f) ? 0 : 1);
        const float K = (D[a] >= 0.0f) ? D[a] : -D[a];
        Cube[w * 3 + 0] += R * K;
        Cube[w * 3 + 1] += G * K;
        Cube[w * 3 + 2] += B * K;
    }
}
inline FVec3 RtCubeLight(const float* Cube, const FVec3& N)
{
    const float D[3] = { N.X, N.Y, N.Z };
    FVec3 Out = { 0.0f, 0.0f, 0.0f };
    for (int a = 0; a < 3; a++)
    {
        const int   w = a * 2 + ((D[a] >= 0.0f) ? 0 : 1);
        const float K = D[a] * D[a];
        Out.X += Cube[w * 3 + 0] * K;
        Out.Y += Cube[w * 3 + 1] * K;
        Out.Z += Cube[w * 3 + 2] * K;
    }
    return Out;
}
// How much of a lamp's light is left at a share of its reach (0 at the lamp, 1 at the reach's end): all of it close
// by, none from the end on, and no edge where it ends.
inline float RtLampFalloff(float Share)
{
    const float S = (Share < 0.0f) ? 0.0f : ((Share > 1.0f) ? 1.0f : Share);
    const float K = 1.0f - S * S;
    return K * K;
}
// A caster's row of the mirror rays' table, in numbers (RDX_RT_GEO_BYTES), and where its parts are: its first vertex,
// its colour (PackRtAlbedo), how much of a texture a unit of the level holds on it (a float's bits), then its light
// along the six ways (PackRtLight each), and from RtGeoTextures on its textures' places in the texture heap plus
// one, by key - 0 where the heap does not hold one.
enum { RtGeoUints = RDX_RT_GEO_BYTES / 4, RtGeoFirst = 0, RtGeoColour = 1, RtGeoDensity = RDX_RT_GEO_DENSITY / 4,
       RtGeoAmbient = RDX_RT_GEO_AMBIENT / 4, RtGeoScale = RDX_RT_GEO_SCALE / 4, RtGeoCube = RDX_RT_GEO_CUBE / 4,
       RtGeoTextures = RDX_RT_GEO_TEXTURES / 4 };

// The ripples things make in water (Water=Low and up): a height field over a square of the world around the camera,
// stepped on the GPU by the wave equation, and pushed wherever something moves through a water surface. RippleTexels
// across RippleWorld units - eight units a texel, small enough for the rings a body makes, large enough to reach the
// far side of most pools. The field steps RippleStepsPerSecond times a second whatever the frame rate, so the rings
// spread at the same speed at 60 and at 200 frames a second.
// The field's size, and how finely the water's surface is cut: RDX_RIPPLE_WORLD_VALUE, RDX_RIPPLE_TEXELS_VALUE and
// RDX_WATER_EDGE_VALUE, in RenderLimits.h - the water's shader takes them as text (FSolidPass) or reads them there
// (dxc).
enum { RippleTexels = RDX_RIPPLE_TEXELS_VALUE, RippleStepsPerSecond = 60, RippleMaxSteps = 3, RippleMaxPulses = 12 };
static const float RippleWorld = RDX_RIPPLE_WORLD_VALUE;
static const float RippleTexel = RippleWorld / (float)RippleTexels;
// The wave equation's two numbers: the speed squared, in texels a step (about 80 units a second at 60 steps), and
// how much of a wave is left after a step (rings fade within two or three seconds).
static const float RippleSpeed2 = 0.03f;
static const float RippleKeep   = 0.985f;

// The field's lower corner for a camera at X (either axis): half the field below it, on the texel grid, so the field moves
// in whole texels and a ring stays where it is in the world while the camera walks.
inline float RippleOrigin(float CameraX)
{
    const float Corner = CameraX - 0.5f * RippleWorld;
    const float Cells  = Corner / RippleTexel;
    const float Whole  = (float)(int)Cells - ((Cells < 0.0f && (float)(int)Cells != Cells) ? 1.0f : 0.0f);
    return Whole * RippleTexel;
}

// How many steps this frame, from the time since the last one: the remainder carries over, and a long stall is not
// made up for (at most RippleMaxSteps), so a loading pause does not end in a burst.
inline int RippleSteps(float& Carry, float Seconds)
{
    if (Seconds < 0.0f)
        Seconds = 0.0f;
    Carry += Seconds * (float)RippleStepsPerSecond;
    int Steps = (int)Carry;
    Carry -= (float)Steps;
    if (Steps > RippleMaxSteps)
    {
        Steps = RippleMaxSteps;
        Carry = 0.0f;
    }
    return Steps;
}

// How hard a thing moving through the surface pushes it, in world units of height a step: its speed along the surface
// (Along, units a second) raises the water where it goes; its speed downwards (Down; negative going up) raises it going
// in and lowers it coming out - so a thing bobbing in place sends rings out and piles nothing up. A push that only ever
// raised the water, every step, grew a mound under it - the field keeps 1 / (1 - RippleKeep) times a steady push - which
// the surface followed up into a cone taller than the crate bobbing there (measured: Laci's FURA save, gone with this).
inline float RipplePush(float Along, float Down)
{
    const float Push = (Along + Down) * 0.004f;
    return (Push > 4.0f) ? 4.0f : ((Push < -4.0f) ? -4.0f : Push);
}

// A raindrop going into the water (RdxWaterRippleSources): a tap as narrow as the field can draw, the water pushed down
// by this much - as hard as a thing falling into it pushes, but for a frame, so its ring runs out and is gone. One
// drop in RainRippleEvery taps: the rain sends hundreds of drops a second into the water around the camera, and the
// rings of all of them, each wider than a real drop's, run into one another and leave a chop with no ring to be seen
// in it.
static const float RainRippleRadius = 8.0f;
static const float RainRipplePush   = 2.0f;
enum { RainRippleEvery = 4 };
// The rings the water's own shader draws for the rain (SolidPass.hlsl, WaterRainRings) are at their full strength from
// this many drops a second going into the water around the camera: a small part of what the game's rain sends down
// around the player, so a shore with only a corner of the rain over water has them as well - below it they fade, and
// with no drop going in there are none.
static const float RainRingFullRate = 40.0f;

// The most the ripples may lift or lower the surface, world units: a bound on the field, whatever pushes it.
static const float RippleLimit = 24.0f;

// How tall the water texture's own waves are made (world units, for the field's full swing): with the water's size across,
// like WaterWaveStrength - a pool's are lower than the sea's, and neither is ever stormy.
inline float WaterWaveHeight(float Across)
{
    const float Low = 2.0f, High = 4.0f;
    if (Across <= 512.0f)
        return Low;
    if (Across >= 4096.0f)
        return High;
    return Low + (High - Low) * (Across - 512.0f) / (4096.0f - 512.0f);
}

// The lamps that glint on the water (Reflections=High and up): how many the shader takes, and how they are ranked. A
// lamp counts when it hangs over a level water surface within twice its light's reach - DistanceToSurface is how far
// that is: the water mirrors a lamp whose light barely gets there, as a pool mirrors the lamps high over it - and the
// nearer, the brighter and the closer to the camera, the better: far water shows little of any one glint. Their rank
// goes by the camera too, so which of them are taken changes as the player walks: the pick's edge is eased
// (PickEdgeShare, WaterLampFade), and no glint comes or goes at once.
enum { WaterLampMax = 8 };
static const float WaterLampFade = 0.25f;
inline float WaterLampRank(float Brightness, float Radius, float DistanceToSurface, float DistanceToCamera)
{
    if (Brightness <= 0.0f || Radius <= 0.0f || DistanceToSurface < 0.0f || DistanceToSurface >= 2.0f * Radius)
        return 0.0f;
    const float Near = 1.0f - DistanceToSurface / (2.0f * Radius);
    return Brightness * Near / (1.0f + DistanceToCamera / 1024.0f);
}

// How many steps a second the water texture's own waves take (FWaterProbe, the texture's MaxFrameRate): the engine
// steps them once a frame when nothing caps them, so they ran three times as fast at 180 frames a second as at 60.
static const float WaterWaveRate = 60.0f;

// The water's splashes: something that falls into a level water surface fast enough throws water up. At most SplashMax at
// a time, SplashParticles drops each; a splash lasts SplashLife seconds, long enough for its highest drop to fall back.
// Slower than SplashMinFall (units a second, downwards) only ripples. The drops' count as a macro too: the splashes'
// shader is compiled from a string and takes it as text (FBackend::EnsureSplashes).
#define RDX_SPLASH_DROPS_VALUE 512
enum { SplashMax = 12, SplashParticles = RDX_SPLASH_DROPS_VALUE };
static const float SplashLife    = 2.0f;
static const float SplashMinFall = 120.0f;
// How hard a splash throws its water up - the fastest drop's start, units a second - from how fast the thing fell and how
// big it is (its collision radius). A body jumping in throws the column about as high as a man; nothing throws it much
// higher than twice that.
inline float SplashStrength(float FallSpeed, float Radius)
{
    float S = 0.45f * FallSpeed;
    S = (S < 90.0f) ? 90.0f : ((S > 420.0f) ? 420.0f : S);
    float Size = (Radius > 0.0f) ? Sqrt(Radius / 20.0f) : 0.7f;
    Size = (Size < 0.7f) ? 0.7f : ((Size > 1.5f) ? 1.5f : Size);
    S *= Size;
    return (S > 380.0f) ? 380.0f : S;
}

// An ornamental fountain's water as drops (FBackend::SetCurtains; the splashes' own passes draw them). They leave the
// slot at the curtain's top one after another, in streams side by side, and fall as a thrown thing falls - so there
// is nothing to simulate. Close together they run into one sheet of water; as they gather speed they draw apart,
// into strands and then beads. At most CurtainMax fountains at a time, the nearest; CurtainStreams streams each,
// CurtainStreamDrops drops of a stream in the air at a time, and CurtainSpray drops thrown up where the water comes
// down. The counts as macros too: the shader is compiled from a string and takes them as text.
#define RDX_CURTAIN_STREAMS_VALUE 128
#define RDX_CURTAIN_STREAM_DROPS_VALUE 32
#define RDX_CURTAIN_SPRAY_VALUE 768
// And the same streams' drops on their way down the fountain's chute to the slot, where it has one: so many of a
// stream on the chute at a time.
#define RDX_CHUTE_STREAM_DROPS_VALUE 96
enum
{
    CurtainMax         = 8,
    CurtainStreams     = RDX_CURTAIN_STREAMS_VALUE,
    CurtainStreamDrops = RDX_CURTAIN_STREAM_DROPS_VALUE,
    CurtainSpray       = RDX_CURTAIN_SPRAY_VALUE,
    ChuteStreamDrops   = RDX_CHUTE_STREAM_DROPS_VALUE,
    CurtainDrops       = CurtainStreams * (CurtainStreamDrops + ChuteStreamDrops) + CurtainSpray,
    // A fountain as it is handed over (RdxWaterFountains): its own numbers, then its chute - four a piece from the
    // slot upwards (which way its water runs on the level as an angle, how much it drops for a unit of that, the
    // water's seconds over it and how far it goes in them), ChutePieces of them, and a row of the whole: the
    // water's seconds down all of it and the pieces' count.
    CurtainOwnFloats   = 8,
    ChutePieces        = 6,
    ChuteRows          = ChutePieces + 1,
    CurtainFloats      = CurtainOwnFloats + ChuteRows * 4,
    CurtainLit         = 3,     // how many of them the laying-on tells from the splashes' water (PSSplash),
    CurtainLitFloats   = 6      // and what it is told of each: the slot's ends and the fall's height
};
// The world's own gravity at the game's scale - sixteen units to the foot - not the engine's, which pulls nearly
// twice as hard: water that falls twice too fast does not look like water. And it leaves the slot at next to no
// speed: it spills over, it is not pushed.
static const float CurtainGravity    = 515.0f;
static const float CurtainStartSpeed = 4.0f;
static const float CurtainReach      = 2048.0f;     // how far from the camera a fountain's drops are still drawn
// How much water a fountain lets down, of the most the drops can be: at 1 its streams are one unbroken sheet, at 0
// thin strands; between the two a sheet that parts and closes as its streams sway. Laci's choice of the three.
static const float CurtainAmount     = 0.5f;
static const float CurtainSpread     = 0.6f;        // how far off its plane a curtain's water leaves the slot
// How long a drop falls from the slot to the pool, Rise units under it.
inline float CurtainFallTime(float Rise)
{
    const float R = (Rise > 1.0f) ? Rise : 1.0f;
    return (Sqrt(CurtainStartSpeed * CurtainStartSpeed + 2.0f * CurtainGravity * R) - CurtainStartSpeed) /
           CurtainGravity;
}

// A liquid's surface among its zone's borders with other zones: a border polygon lying level - its plane's normal
// within WaterSurfaceLevel of straight up or down - with the liquid UNDER it. A zone's other borders are no surface: an
// upright pane between the water and a room, a hatch in a flooded room's floor. bLiquidBehind: the liquid is on the
// plane's back, the side its normal points away from. And the border polygons of a zone within WaterSurfaceStep of one
// another in height are one surface - a river's, laid in pieces a unit or two apart - while two basins of one zone, or
// the sea and a pool in a base under it, are two.
static const float WaterSurfaceLevel = 0.99f;
static const float WaterSurfaceStep  = 8.0f;
inline bool WaterBorderIsSurface(float PlaneZ, bool bLiquidBehind)
{
    return bLiquidBehind ? (PlaneZ >= WaterSurfaceLevel) : (PlaneZ <= -WaterSurfaceLevel);
}
// A sea is wide. The level's water that ends in sky is its sea (EndlessWater) only where its surface is at least
// WaterSeaLeastSide across both ways: a roof's water tank or a basin with a sky window beside it is not one, and a
// ring of water running on from it to the horizon would hang in the air over the level.
static const float WaterSeaLeastSide = 2000.0f;
inline bool WaterIsSeaSized(float Wide, float Long)
{
    return Wide >= WaterSeaLeastSide && Long >= WaterSeaLeastSide;
}

// A lamp seen across a level water surface, the eye on one side of it and the lamp on the other: how much of the
// lamp's light comes straight through. CosUp: how steeply the line between the two crosses the surface - the cosine
// of its angle from straight up. From under the water the surface lets the outside in only through a window
// overhead: past the angle at which the light can no longer leave the water it is a mirror, and nothing of the
// lamp comes. Of the rest the surface gives a part back, as the water's own sheen has it - a fiftieth head-on, all
// of it along the surface - and that part is lost to the eye.
static const float WaterIndex = 1.333f;
inline float WaterLampThrough(float CosUp, bool bFromBelow)
{
    float C = (CosUp < 0.0f) ? -CosUp : CosUp;
    if (C > 1.0f)
        C = 1.0f;
    float CosAir = C;
    if (bFromBelow)
    {
        const float SinAirSq = (1.0f - C * C) * WaterIndex * WaterIndex;
        if (SinAirSq >= 1.0f)
            return 0.0f;
        CosAir = Sqrt(1.0f - SinAirSq);
    }
    const float Off  = 1.0f - CosAir;
    const float Back = 0.02f + 0.98f * Off * Off * Off * Off * Off;
    return 1.0f - Back;
}

// ---------------------------------------------------------------------------
// A water polygon cut up before the GPU lifts it (WaterCutPolygon)
// ---------------------------------------------------------------------------
//
// The GPU cuts each triangle of a water polygon into at most 64 pieces along an edge - evenly along the polygon, not
// along the screen. A level's water is often a few polygons hundreds of units long: seen flat, or through a scope,
// one of them fills the picture, and 64 even pieces of it are tens of units each - while the waves that lift the
// surface are a few units long: the points cut out of it rise and fall with no bearing on one another, and the
// crests stand out as rows of large jagged triangles. So a polygon is cut beforehand, in the view's own space, by
// planes that are the same for every polygon of a view - two polygons sharing an edge are cut at the same places on
// it:
//   - planes of one depth, each WaterBandStep times as far as the one before: a band is then shallow enough for 64
//     cuts of it to follow the waves - seen flat it is the way into the picture that is far too long for them;
//   - planes through the eye along the screen's columns and rows, WaterCutSpan pixels apart, for the same across.
// What the GPU makes of a piece's edge is the shader's rule (WaterPatchConstants). The depth planes go as far as the
// lifting does (WaterLiftEnd): what is not lifted needs no cutting. The screen's lines go as far as the picture and
// WaterCutBeyond pixels past its sides - lifted, a point just off the picture can come into it. Farther off a
// polygon is left in one piece to a band: not every polygon is clipped to the picture - a sea running on to the
// horizon is not -, and the lines across the whole of such a one are past counting.
static const float WaterBandFirst = 8.0f;
static const float WaterBandStep  = 1.25f;
static const float WaterCutSpan   = 256.0f;
static const float WaterCutBeyond = 512.0f;
static const float WaterCutSlack  = 0.001f;     // a corner this near a plane, in the level's units, is on it

// How far the waves' lifting reaches: none beyond this many units from the eye. The shader's own rule
// (WaterLiftFade), said here for the cutting.
static const float WaterLiftEnd = 1600.0f;

// A convex polygon cut in two by a plane (Nx, Ny, Nz, D: where N.P + D is zero), each part in the order the polygon
// went round: Front what lies where N.P + D is positive, Back the rest; a corner on the plane - within Slack of it -
// goes to both. A corner is Stride numbers: the first three its place, the last a mark of the caller's own, the ones
// between carried along. A new corner, on an edge the plane crosses, takes those by its place between the edge's
// ends and has 0 for its mark; it is worked out from the edge's two ends in one order whichever way the polygon goes
// round, so two polygons that share the edge get the very same corner. Front and Back need room for Count + 2
// corners each; under three corners a part is no polygon.
template <int Stride>
inline void CutConvex(const float* In, int Count, float Nx, float Ny, float Nz, float D, float Slack,
                      float* Front, int& FrontCount, float* Back, int& BackCount)
{
    FrontCount = 0;
    BackCount  = 0;
    for (int i = 0; i < Count; i++)
    {
        const float* const A  = In + i * Stride;
        const float* const B  = In + ((i + 1) % Count) * Stride;
        const float        Da = Nx * A[0] + Ny * A[1] + Nz * A[2] + D;
        const float        Db = Nx * B[0] + Ny * B[1] + Nz * B[2] + D;
        const int          Sa = (Da > Slack) ? 1 : ((Da < -Slack) ? -1 : 0);
        const int          Sb = (Db > Slack) ? 1 : ((Db < -Slack) ? -1 : 0);
        if (Sa >= 0)
        {
            for (int k = 0; k < Stride; k++)
                Front[FrontCount * Stride + k] = A[k];
            FrontCount++;
        }
        if (Sa <= 0)
        {
            for (int k = 0; k < Stride; k++)
                Back[BackCount * Stride + k] = A[k];
            BackCount++;
        }
        if (Sa * Sb < 0)
        {
            const bool bTurn = (B[0] < A[0]) ||
                               (B[0] == A[0] && (B[1] < A[1] || (B[1] == A[1] && B[2] < A[2])));
            const float* const From = bTurn ? B : A;
            const float* const To   = bTurn ? A : B;
            const float        Df   = bTurn ? Db : Da;
            const float        Dt   = bTurn ? Da : Db;
            const float        T    = Df / (Df - Dt);
            float* const F = Front + FrontCount * Stride;
            float* const K = Back + BackCount * Stride;
            for (int k = 0; k < Stride - 1; k++)
                F[k] = K[k] = From[k] + (To[k] - From[k]) * T;
            F[Stride - 1] = K[Stride - 1] = 0.0f;
            FrontCount++;
            BackCount++;
        }
    }
}

// The most corners a polygon handed to WaterCutPolygon may have, and the most a piece of it can: the polygon's own,
// and two for each of the six planes a piece lies between.
enum { WaterCutMostIn = 36, WaterCutMostCorners = WaterCutMostIn + 12 };

// The view a polygon is cut for: where its axis meets the screen (CentreX, CentreY) and the engine's projection
// (Focal), and the part of the screen its picture fills - all in pixels.
struct FWaterCutView
{
    float CentreX, CentreY, Focal;
    float Left, Top, Right, Bottom;
};

// The most pieces WaterCutPolygon makes of a polygon in a view, whatever the polygon: each of the bands there are
// planes for, by each cell of the screen's lines. The room for the pieces never needs to be more than for these.
inline int WaterCutMostPieces(const FWaterCutView& View, float Reach)
{
    int Bands = 2;      // before the first plane, and past the last
    for (float Depth = WaterBandFirst; Depth < Reach; Depth *= WaterBandStep)
        Bands++;
    const float Wide   = View.Right - View.Left + 2.0f * WaterCutBeyond;
    const float Tall   = View.Bottom - View.Top + 2.0f * WaterCutBeyond;
    const int   Across = (Wide > 0.0f) ? (int)(Wide / WaterCutSpan) + 2 : 1;
    const int   Down   = (Tall > 0.0f) ? (int)(Tall / WaterCutSpan) + 2 : 1;
    return Bands * Across * Down;
}

// What WaterCutPolygon's pieces are written into: Out takes their corners one piece after another, Sizes each
// piece's number of corners. bFull: there was not room for all of them.
struct FWaterCutRoom
{
    float* Out;
    int    OutRoom;         // in corners
    int*   Sizes;
    int    SizesRoom;
    int    Pieces;
    int    Used;
    bool   bFull;
};
template <int Stride>
inline void WaterCutKeep(FWaterCutRoom& Room, const float* Piece, int Count)
{
    if (Count < 3)
        return;
    if (Room.Pieces >= Room.SizesRoom || Room.Used + Count > Room.OutRoom)
    {
        Room.bFull = true;
        return;
    }
    for (int k = 0; k < Count * Stride; k++)
        Room.Out[Room.Used * Stride + k] = Piece[k];
    Room.Sizes[Room.Pieces++] = Count;
    Room.Used += Count;
}
// A piece cut along the screen: bDown false by the columns - what is left of each goes on to the rows -, true by the
// rows, whose parts are kept. A plane through the eye along the screen's line S is where the place across (x, or y)
// is (S - Centre) / Focal times the depth. Only the view's own lines cut (WaterCutBeyond): what of the piece lies
// past the last of them to a side stays one part.
template <int Stride>
inline void WaterCutAlongScreen(FWaterCutRoom& Room, const float* Piece, int Count, bool bDown,
                                const FWaterCutView& View)
{
    const int   Axis   = bDown ? 1 : 0;
    const float Centre = bDown ? View.CentreY : View.CentreX;
    const float First  = (bDown ? View.Top : View.Left) - WaterCutBeyond;
    const float Last   = (bDown ? View.Bottom : View.Right) + WaterCutBeyond;
    float Rest[WaterCutMostCorners * Stride], Before[WaterCutMostCorners * Stride], After[WaterCutMostCorners * Stride];
    int   RestCount = Count;
    for (int k = 0; k < Count * Stride; k++)
        Rest[k] = Piece[k];
    float Least = 0.0f, Most = 0.0f;
    for (int i = 0; i < Count; i++)
    {
        const float Z = (Piece[i * Stride + 2] > 0.001f) ? Piece[i * Stride + 2] : 0.001f;
        const float S = Centre + Piece[i * Stride + Axis] * View.Focal / Z;
        Least = (i == 0 || S < Least) ? S : Least;
        Most  = (i == 0 || S > Most) ? S : Most;
    }
    if (Most > First && Least < Last)
    {
        // From the first of the view's lines at or past the piece's near side; a line right at a side of the piece
        // cuts nothing off.
        const float From = (Least > First) ? Least : First;
        int         Step = (int)(From / WaterCutSpan);
        if ((float)Step * WaterCutSpan < From)
            Step++;
        for (float Line = (float)Step * WaterCutSpan;
             RestCount >= 3 && Line < Most - 0.01f && Line <= Last && RestCount + 2 <= WaterCutMostCorners;
             Line += WaterCutSpan)
        {
            if (Line <= Least + 0.01f)
                continue;
            int BeforeCount = 0, AfterCount = 0;
            const float Lean = (Line - Centre) / View.Focal;
            CutConvex<Stride>(Rest, RestCount, bDown ? 0.0f : 1.0f, bDown ? 1.0f : 0.0f, -Lean, 0.0f, WaterCutSlack,
                              After, AfterCount, Before, BeforeCount);
            if (BeforeCount >= 3)
            {
                if (bDown)
                    WaterCutKeep<Stride>(Room, Before, BeforeCount);
                else
                    WaterCutAlongScreen<Stride>(Room, Before, BeforeCount, true, View);
            }
            RestCount = (AfterCount >= 3) ? AfterCount : 0;
            for (int k = 0; k < RestCount * Stride; k++)
                Rest[k] = After[k];
        }
    }
    if (RestCount >= 3)
    {
        if (bDown)
            WaterCutKeep<Stride>(Room, Rest, RestCount);
        else
            WaterCutAlongScreen<Stride>(Room, Rest, RestCount, true, View);
    }
}

// A water polygon cut by the view's planes (above). In: Count corners as CutConvex takes them, in the view's space -
// x to the right, y down, z the depth - each one's mark its number among them, from 1. Reach: how far the depth
// planes go. The pieces' corners go into Out one piece after another (Stride numbers a corner), each piece's count
// into Sizes: a corner that was the polygon's own keeps its mark, a new one has 0. Returns how many pieces - one
// where no plane crosses the polygon -, or -1 where the room given was too little (WaterCutMostPieces: how much is
// always enough) or the polygon has more corners than WaterCutMostIn: the caller makes more room, or hands the
// polygon over as it is.
template <int Stride>
inline int WaterCutPolygon(const float* In, int Count, const FWaterCutView& View, float Reach, float* Out,
                           int OutRoom, int* Sizes, int SizesRoom)
{
    if (Count < 3 || Count > WaterCutMostIn || !(View.Focal > 0.0f))
        return -1;
    FWaterCutRoom Room;
    Room.Out       = Out;
    Room.OutRoom   = OutRoom;
    Room.Sizes     = Sizes;
    Room.SizesRoom = SizesRoom;
    Room.Pieces    = 0;
    Room.Used      = 0;
    Room.bFull     = false;
    float Rest[WaterCutMostCorners * Stride], Near[WaterCutMostCorners * Stride], Far[WaterCutMostCorners * Stride];
    int   RestCount = Count;
    float Least = In[2], Most = In[2];
    for (int i = 0; i < Count; i++)
    {
        for (int k = 0; k < Stride; k++)
            Rest[i * Stride + k] = In[i * Stride + k];
        Least = (In[i * Stride + 2] < Least) ? In[i * Stride + 2] : Least;
        Most  = (In[i * Stride + 2] > Most) ? In[i * Stride + 2] : Most;
    }
    // The bands' planes one after another from the first, the same numbers for every polygon: those the polygon lies
    // across, as far as the first one past the reach.
    float Depth = WaterBandFirst;
    for (; RestCount >= 3 && RestCount + 2 <= WaterCutMostCorners && Depth < Most - WaterCutSlack;
         Depth *= WaterBandStep)
    {
        if (Depth > Least + WaterCutSlack)
        {
            int NearCount = 0, FarCount = 0;
            CutConvex<Stride>(Rest, RestCount, 0.0f, 0.0f, 1.0f, -Depth, WaterCutSlack, Far, FarCount, Near,
                              NearCount);
            if (NearCount >= 3)
                WaterCutAlongScreen<Stride>(Room, Near, NearCount, false, View);
            RestCount = (FarCount >= 3) ? FarCount : 0;
            for (int k = 0; k < RestCount * Stride; k++)
                Rest[k] = Far[k];
        }
        if (Depth >= Reach)
            break;
    }
    if (RestCount >= 3)
        WaterCutAlongScreen<Stride>(Room, Rest, RestCount, false, View);
    return Room.bFull ? -1 : Room.Pieces;
}

// Whether Text holds Want anywhere, in any case. Want is in small letters.
template <typename CH>
inline bool TextHas(const CH* Text, const char* Want)
{
    if (!Text)
        return false;
    for (const CH* P = Text; *P; P++)
    {
        int n = 0;
        for (; Want[n] != 0; n++)
        {
            // Past the text's end its closing zero is no letter of the word, and the comparison stops there.
            CH Ch = P[n];
            if (Ch >= (CH)'A' && Ch <= (CH)'Z')
                Ch = (CH)(Ch - (CH)'A' + (CH)'a');
            if (Ch != (CH)Want[n])
                break;
        }
        if (Want[n] == 0)
            return true;
    }
    return false;
}

// A waterfall's sheet, by what its texture is called and how it is laid (the render device asks this of a sheet that
// stands, lets things through and is drawn over what is behind it). A texture named for falling water - fall, cascade,
// spout, fountain - is a fall as it is. One named only for water - water, wtr - is a fall where the level makes it
// run down by itself: the same texture standing still is a tank's side or a tube of liquid, not falling water. And a
// sheet that runs down but is named for neither is not water at all: the scrolling screens of a computer room are
// made so.
enum EFallName
{
    FallName_None = 0,
    FallName_Water,
    FallName_Falling
};
template <typename CH>
inline EFallName FallNameKind(const CH* Name)
{
    if (TextHas(Name, "fall") || TextHas(Name, "cascade") || TextHas(Name, "spout") || TextHas(Name, "fountain"))
        return FallName_Falling;
    if (TextHas(Name, "water") || TextHas(Name, "wtr"))
        return FallName_Water;
    return FallName_None;
}
inline bool FallSheetByName(EFallName Name, bool bRunsDown)
{
    return Name == FallName_Falling || (Name == FallName_Water && bRunsDown);
}
// Whether a fall's sheet is an ornamental fountain's: a thin curtain of water let down a pane and lit from its top,
// not a stream's fall - drawn as one (the shader's FountainSurface).
template <typename CH>
inline bool IsFountainName(const CH* Name)
{
    return TextHas(Name, "fountain");
}
// An ornamental fountain's pool: the level's see-through surface its curtain comes down into. Such a pool is often
// no liquid's border - a sheet of the water's texture over a basin, with no water zone under it - so the level's
// water does not know of it, and it is found from the fountain: a level surface at the height of the curtain's
// lowest edge, with that edge's middle over it.
//   Reach   how far above or below the edge the surface may lie
//   Margin  how far outside the surface's extent the edge's middle may lie
static const float FountainPoolReach  = 4.0f;
static const float FountainPoolMargin = 2.0f;
inline bool FountainFootInPool(float FootX, float FootY, float FootZ, float MinX, float MinY, float MaxX, float MaxY,
                               float PoolZ)
{
    const float Dz = FootZ - PoolZ;
    return Dz <= FountainPoolReach && Dz >= -FountainPoolReach && FootX >= MinX - FountainPoolMargin &&
           FootX <= MaxX + FountainPoolMargin && FootY >= MinY - FountainPoolMargin &&
           FootY <= MaxY + FountainPoolMargin;
}
// Whether a level surface carries a pool on: the same water at the same height, its extent touching the pool's - a
// basin's surface is often cut into several. Asked only of surfaces of the pool's own texture.
inline bool FountainPoolCarriesOn(float MinX, float MinY, float MaxX, float MaxY, float Z, float PoolMinX,
                                  float PoolMinY, float PoolMaxX, float PoolMaxY, float PoolZ)
{
    const float Dz = Z - PoolZ;
    return Dz <= 1.0f && Dz >= -1.0f && MinX <= PoolMaxX + 1.0f && MaxX >= PoolMinX - 1.0f &&
           MinY <= PoolMaxY + 1.0f && MaxY >= PoolMinY - 1.0f;
}
// What a pool's run tells the shader in the place of the texture's own waves (the water constants' WaterWave.x):
// that a fountain's curtain comes down into it.
static const float FountainPoolSheet = -1.0f;

// An ornamental fountain's chute: the lying pieces of the fountain's own surface its water runs down on its way to
// the slot a curtain falls from - sloping or level, one after another. They are water as the level's other water
// is, its surface really lifted - but running water: what its surface is uneven by, the water carries down from
// piece to piece as far as the slot. A piece's run tells the shader so where a pool's says it is a pool.
static const float FountainChuteSheet = -2.0f;
// How fast the water runs on a piece that drops Drop units for a unit along the level: on a level stretch not much
// slower than on a slope - a chute's stretches are short, and the water comes onto one with the speed it has - and
// the faster the steeper, by the root of the drop, as water down a slope gathers speed. Units a second, along the
// piece. The shader lays its humps out for water no slower than ChuteLevelSpeed (SolidPass.hlsl, ChuteSize).
static const float ChuteLevelSpeed = 8.0f;
static const float ChuteSlopeSpeed = 6.0f;
// A piece that drops less than this for a unit is level: its water runs the way the piece below it does.
static const float ChuteLevelDrop  = 0.02f;
inline float ChuteSpeed(float Drop)
{
    return ChuteLevelSpeed + ChuteSlopeSpeed * Sqrt((Drop > 0.0f) ? Drop : 0.0f);
}
// How much of the film lies on a piece: Along units of the level, dropping Drop for each.
inline float ChuteLength(float Along, float Drop)
{
    const float D = (Drop > 0.0f) ? Drop : 0.0f;
    return ((Along > 0.0f) ? Along : 0.0f) * Sqrt(1.0f + D * D);
}
// How long the water takes over that piece, in seconds.
inline float ChuteTime(float Along, float Drop)
{
    return ChuteLength(Along, Drop) / ChuteSpeed(Drop);
}
// Whether a chute's piece starts at a curtain's slot: its lowest edge at the height of the curtain's top, with that
// top's middle over the piece.
inline bool ChuteStartsAt(float LowZ, float MinX, float MinY, float MaxX, float MaxY, float TopZ, float TopX,
                          float TopY)
{
    const float Dz = LowZ - TopZ;
    return Dz <= 1.0f && Dz >= -1.0f && TopX >= MinX - 2.0f && TopX <= MaxX + 2.0f && TopY >= MinY - 2.0f &&
           TopY <= MaxY + 2.0f;
}
// Whether a piece carries a chute on upwards from another: its lowest edge at the height of the other's highest,
// their extents on the level touching.
inline bool ChuteCarriesOn(float LowZ, float MinX, float MinY, float MaxX, float MaxY, float BelowHighZ,
                           float BelowMinX, float BelowMinY, float BelowMaxX, float BelowMaxY)
{
    const float Dz = LowZ - BelowHighZ;
    return Dz <= 1.0f && Dz >= -1.0f && MinX <= BelowMaxX + 1.0f && MaxX >= BelowMinX - 1.0f &&
           MinY <= BelowMaxY + 1.0f && MaxY >= BelowMinY - 1.0f;
}

// Whether two of a fall's sheets are one column of water: they share a quarter of the lower one's height, and stand on
// one another's middle - crossed, as a fall is built to look round.
inline bool FallsJoin(const float* MidA, float HalfA, float BottomA, float TopA,
                      const float* MidB, float HalfB, float BottomB, float TopB)
{
    const float Shared = ((TopA < TopB) ? TopA : TopB) - ((BottomA > BottomB) ? BottomA : BottomB);
    const float TallA  = TopA - BottomA;
    const float TallB  = TopB - BottomB;
    if (Shared < 0.25f * ((TallA < TallB) ? TallA : TallB))
        return false;
    const float dx    = MidA[0] - MidB[0];
    const float dy    = MidA[1] - MidB[1];
    const float Reach = 0.5f * (HalfA + HalfB);
    return dx * dx + dy * dy <= Reach * Reach;
}

// A waterfall's sheet, gathered instead of drawn (Water=Low and up): its middle on the level, the direction across
// it and its half-width that way, how far down and up it reaches, the hue of the water it comes down into, and
// whether it comes down into water.
struct FFallSheetPiece
{
    float Mid[2];
    float Across[2];
    float Half;
    float Bottom, Top;
    float Hue[3];
    int   bFoot;
};
// How many sheets a frame holds, and how many numbers a column of water is to the walk (FVolumetrics::FALL_FLOATS).
// The sheets' room is a fixed one, and so is the columns' (FVolumetrics::MAX_FALLS), only because the game's levels
// come nowhere near them and a fall past them is still drawn, as its sheets. A room the game can fill flips what
// does not fit between two pictures as the view moves: scripts\level_rules_check.py holds every level's falls
// against both numbers.
enum { FallPiecesMost = 64, FallColumnFloats = 16 };

inline bool FallPiecesJoin(const FFallSheetPiece& A, const FFallSheetPiece& B)
{
    return FallsJoin(A.Mid, A.Half, A.Bottom, A.Top, B.Mid, B.Half, B.Bottom, B.Top);
}

// Which column each of a level's fall sheets stands in: the sheets that are one column, and those that are one with
// them in turn. Column gets, for every sheet, the number of its column, the columns numbered in the order of their
// first sheets; the answer is how many there are. The level's own, whichever of the sheets a frame draws: a column
// worked out from what a frame happened to draw moved and changed shape as its sheets came into the view.
inline int FallColumnsOf(const FFallSheetPiece* Pieces, int Count, int* Column)
{
    for (int i = 0; i < Count; i++)
        Column[i] = i;
    for (int i = 0; i < Count; i++)
        for (int j = i + 1; j < Count; j++)
            if (FallPiecesJoin(Pieces[i], Pieces[j]))
            {
                const int A = FlameRoot(Column, i), B = FlameRoot(Column, j);
                if (A < B)
                    Column[B] = A;
                else if (B < A)
                    Column[A] = B;
            }
    for (int i = 0; i < Count; i++)
        Column[i] = FlameRoot(Column, i);
    int Columns = 0;
    for (int i = 0; i < Count; i++)
        Column[i] = (Column[i] == i) ? Columns++ : Column[Column[i]];
    return Columns;
}

// The frame's sheets of falling water. A fall is taken whole - all the sheets of a column, as the level has them
// (FallColumnsOf) - or not at all, and each sheet is counted into its column as it comes (Group, FlameBodyAdd): a
// fall the walk has no room for is known while its sheets can still be drawn.
struct FFallGather
{
    FFallSheetPiece Pieces[FallPiecesMost];
    int             Group[FallPiecesMost];
    int             Count;          // the sheets taken
    int             Columns;        // the columns they make

    FFallGather() : Count(0), Columns(0) {}
    void Begin()
    {
        Count   = 0;
        Columns = 0;
    }
    // False where the fall is not taken: no room for all its sheets, or for one more column where it stands with
    // none of those taken so far (MostColumns).
    bool AddTogether(const FFallSheetPiece* P, int N, int MostColumns)
    {
        if (N <= 0 || N > FallPiecesMost)
            return false;
        bool bStands = false;       // with a sheet already taken: then it makes no new column
        for (int k = 0; k < N && !bStands; k++)
            for (int i = 0; i < Count && !bStands; i++)
                bStands = FallPiecesJoin(Pieces[i], P[k]);
        if (Count + N > FallPiecesMost || (!bStands && Columns >= MostColumns))
            return false;
        for (int k = 0; k < N; k++)
        {
            int Stands[FallPiecesMost];
            int StandCount = 0;
            for (int i = 0; i < Count; i++)
                if (FallPiecesJoin(Pieces[i], P[k]))
                    Stands[StandCount++] = i;
            Pieces[Count] = P[k];
            FlameBodyAdd(Group, Count, Columns, Stands, StandCount);
            Count++;
        }
        return true;
    }
};

// The columns of falling water the walk takes (FallColumnFloats numbers each: the middle on the level, the foot and
// the top; the direction along it and its half-width along and across; the light the water gives back, and whether
// it comes down into water; the foam's light, and the glint's share of the lamp) from a frame's sheets. Sheets that
// reach the same heights and stand on one another's middle - crossed, as a fall is built to look round - are one
// column, its middle theirs averaged: round where any two of them lie different ways, as wide as
// WaterfallColumnWidth of the widest; a ribbon along the first of them where they all run the same way, as a sheet
// on its own does. Nothing in it goes by the order the sheets come in, but the ribbon's direction - and a level's
// fall is handed over in one order.
inline int FallColumnsMake(const FFallSheetPiece* Pieces, int Count, float* Out, int MaxOut)
{
    int Group[FallPiecesMost];
    Count = (Count < FallPiecesMost) ? Count : (int)FallPiecesMost;
    for (int i = 0; i < Count; i++)
        Group[i] = i;
    for (int i = 0; i < Count; i++)
        for (int j = i + 1; j < Count; j++)
            if (FallPiecesJoin(Pieces[i], Pieces[j]))
                Group[FlameRoot(Group, i)] = FlameRoot(Group, j);
    int N = 0;
    for (int r = 0; r < Count && N < MaxOut; r++)
    {
        if (FlameRoot(Group, r) != r)
            continue;
        int   Members[FallPiecesMost];
        int   MemberCount = 0;
        float Mid[2] = { 0.0f, 0.0f }, Hue[3] = { 0.0f, 0.0f, 0.0f };
        float Half = 0.0f, Bottom = 1e9f, Top = -1e9f;
        bool  bFoot = false, bRound = false;
        for (int m = 0; m < Count; m++)
        {
            if (FlameRoot(Group, m) != r)
                continue;
            const FFallSheetPiece& P = Pieces[m];
            // Any two that lie different ways make it round.
            for (int k = 0; k < MemberCount && !bRound; k++)
            {
                const FFallSheetPiece& Q = Pieces[Members[k]];
                const float Along = P.Across[0] * Q.Across[0] + P.Across[1] * Q.Across[1];
                bRound = Along < 0.9f && Along > -0.9f;
            }
            Members[MemberCount++] = m;
            Mid[0] += P.Mid[0];
            Mid[1] += P.Mid[1];
            for (int k = 0; k < 3; k++)
                Hue[k] += P.Hue[k];
            Half   = (P.Half > Half) ? P.Half : Half;
            Bottom = (P.Bottom < Bottom) ? P.Bottom : Bottom;
            Top    = (P.Top > Top) ? P.Top : Top;
            bFoot  = bFoot || P.bFoot != 0;
        }
        const float* const Dir = Pieces[Members[0]].Across;
        float* const       F   = Out + N * FallColumnFloats;
        F[0] = Mid[0] / (float)MemberCount;
        F[1] = Mid[1] / (float)MemberCount;
        F[2] = Bottom;
        F[3] = Top;
        F[4] = Dir[0];
        F[5] = Dir[1];
        if (bRound)
            F[6] = F[7] = Half * WaterfallColumnWidth;
        else
        {
            const float Thick = Half * WaterfallRibbonWidth * WaterfallRibbonThick;
            F[6] = Half * WaterfallRibbonWidth;
            F[7] = (Thick > WaterfallRibbonMin) ? Thick : WaterfallRibbonMin;
        }
        for (int k = 0; k < 3; k++)
        {
            const float H = Hue[k] / (float)MemberCount;
            F[8 + k]  = H * WaterfallAlbedo;
            F[12 + k] = (0.3f * H + 0.7f) * WaterfallFoamLight;     // white, touched by the water's hue
        }
        F[11] = bFoot ? 1.0f : 0.0f;
        F[15] = WaterfallGlint;
        N++;
    }
    return N;
}

// How strong the waves are on a body of water, from its size across (world units): a pool is calmer than the sea. The
// slope of the steepest wave, before the four waves are summed - so the refraction stays gentle everywhere.
inline float WaterWaveStrength(float Across)
{
    const float Small = 0.030f, Large = 0.060f;
    if (Across <= 512.0f)
        return Small;
    if (Across >= 4096.0f)
        return Large;
    return Small + (Large - Small) * (Across - 512.0f) / (4096.0f - 512.0f);
}

// What counts as near, in the world's units. JC stands about ninety units tall, so this is a third of him - a little
// above the knee: enough for the crease along a wall's foot and the ground around a crate, too little for a wall at the
// far side of a room to darken the middle of its floor. 24, 32 and 48 were measured to cost the same (0.43 to 0.45 ms
// on one scene); 24 left the contact darkening at a crate's foot a thin line, 32 gives it some width.
static const float AoRadius = 32.0f;
// How strong the occlusion is, as a CONTRAST: the compensation lifts the frame as a whole back up, so this sets how much
// darker a crease comes out than the open floor, not how dark the picture gets. Above 1 because the taps' average is
// small even where the occlusion is real - a wall beside the floor takes about half of the samples, and each of those
// only part of its share. At 1 the darkest crease came out barely under 0.9, invisible in the picture; 3 takes a
// right-angled crease to about half, which is what one should be.
static const float AoStrength = 3.0f;
// The RT occlusion's own (AmbientOcclusion=RT): its rays find everything within the radius - under the desk, behind the
// chair, what the screen does not show - so the same strength shuts in far more than the SSAO does, and it puts its
// darkness where the geometry is rather than along every edge of the picture.
//
// Measured with the occlusion shown alone (1 open, 0 shut), against the SSAO's 3.0 on the same view: the SSAO's mean is
// 0.922, its darkest tenth 0.706, its darkest fiftieth 0.345. The rays at 0.7 gave 0.952 / 0.824 / 0.694 - a hint of
// what the SSAO does, which is what it looked like; at 1.2 the mean and the darkest tenth meet the SSAO's (0.918 /
// 0.702) and the deepest corners stay lighter; at 1.8 the deepest meet it (0.361) and the middle goes past it. 1.5 is
// Laci's pick from those four pictures: the corners carry the SSAO's weight, with nothing overdarkened.
static const float RtAoStrength = 1.5f;
// A neighbour counts only once it rises this far out of the surface's own plane, as the cosine of its direction against
// the normal. Without it a flat wall seen at a slant occludes itself: its depth, read back pixel by pixel, wobbles by a
// hair either side of true, and half of those hairs point outwards.
static const float AoCosBias = 0.1f;
// The samples a pixel takes - a macro and not only a constant, because the shader's text needs it pasted in
// (GFinalAoShader, RDX_STRINGIFY), exactly as RDX_SHADOW_MAX_LIGHTS is: one number, so the two cannot drift apart.
#define RDX_AO_SAMPLES 12
static const int AoSamples = RDX_AO_SAMPLES;

// One neighbour's share of the occlusion, exactly as GFinalAoShader works it out - here so that the curve can be
// checked on the CPU. V is the step from the surface to the neighbour, N the surface's unit normal. Nothing from a
// neighbour in or behind the surface's own plane, nothing from one beyond the radius, and the most from one close by
// and straight out from the surface.
inline float AoTap(float Vx, float Vy, float Vz, float Nx, float Ny, float Nz, float Radius)
{
    const float VV = Vx * Vx + Vy * Vy + Vz * Vz;
    const float R2 = Radius * Radius;
    if (!(VV < R2) || VV < 1.0e-6f)
        return 0.0f;
    const float C = (Vx * Nx + Vy * Ny + Vz * Nz) / Sqrt(VV);
    float T = (C - AoCosBias) / (1.0f - AoCosBias);
    T = (T < 0.0f) ? 0.0f : ((T > 1.0f) ? 1.0f : T);
    return T * (1.0f - VV / R2);
}

// Where the occlusion stops falling straight (1 - AoStrength x the taps' average) and bends towards black slowly:
// K / (1 + (K - A) / K), the same value and the same slope at the knee, and never quite black. Kept straight, a third of
// the taps would shut a pixel to black, with the taps' own noise coming through at the full strength: where something
// floats a hand's width in front of a surface - the menu's logo before its glow, a body before a wall - the samples fall
// on it and off it from pixel to pixel, a dark, grainy hole. A right-angled crease (about 0.5) stays on the straight
// part. A macro as well, for the shader's text (GFinalAoShader).
#define RDX_AO_KNEE 0.4f
static const float AoKnee = RDX_AO_KNEE;

// The occlusion from the taps' sum: 1 open, falling straight with AoStrength down to AoKnee, then slowly towards black.
inline float AoFromSum(float Sum, int Count)
{
    if (Count <= 0)
        return 1.0f;
    const float A = 1.0f - AoStrength * Sum / (float)Count;
    if (A >= AoKnee)
        return (A > 1.0f) ? 1.0f : A;
    return AoKnee / (1.0f + (AoKnee - A) / AoKnee);
}

// The two AO passes' root constants - GFinalAoShader's and GFinalAoBlurShader's cbuffer, written down once, here, and
// read from here by FBackend::FillAoConstants and by the test that holds the three together. It opens as the shadow
// factor's own constants do - the tangents, the scale, the view and the rectangles to leave out - because the occlusion
// must leave out exactly what the shadows do: a view drawn later onto part of the screen, and whatever is drawn on top
// of the world, the weapon in the player's hand first of all.
enum
{
    AoCbTanX      = 0,
    AoCbTanY      = 1,
    AoCbScale     = 2,
    AoCbNoOnTop   = 3,
    AoCbViewRect  = 4,     // four values
    AoCbSkipRect  = 8,     // four rectangles of four
    AoCbSkipCount = 24,
    AoCbRadius    = 25,
    AoCbStrength  = 26,
    AoCbCosBias   = 27,
    AoCbCount     = 28     // seven registers of four
};
static const int AoSkipRects = 4;

// ---------------------------------------------------------------------------
// The key that opens the renderer's menu - DebugKey in the settings file. Scroll Lock unless changed: the game binds
// nothing to it, so taking it costs the game nothing. Plenty of keyboards have none (laptops, compact ones), so the key
// can be named: by one of the names below, as F1 to F24, as a single letter or digit, or by its Windows virtual-key
// code, decimal or in hex with 0x.
//
// Whatever key it is, the renderer takes it before the game does, so a key the game had bound loses that binding. A few
// keys cannot open the menu at all, and are refused with the reason (MenuKeyRefusal).
// ---------------------------------------------------------------------------
static const int MenuKeyDefault = 0x91;      // VK_SCROLL

struct FMenuKeyName
{
    int         Vk;
    const char* Ini;        // how the settings file spells it; the first row of a key is how it is written out
    const char* Caption;    // how the screen says it - the caption font has capitals, digits and a few signs
};

inline const FMenuKeyName* MenuKeyNames(int& Count)
{
    static const FMenuKeyName Names[] =
    {
        { 0x91, "ScrollLock", "SCROLL LOCK" },
        { 0x91, "ScrLk",      "SCROLL LOCK" },
        { 0x91, "Scroll",     "SCROLL LOCK" },
        { 0x13, "Pause",      "PAUSE" },
        { 0x13, "Break",      "PAUSE" },
        { 0x2D, "Insert",     "INSERT" },
        { 0x2D, "Ins",        "INSERT" },
        { 0x2E, "Delete",     "DELETE" },
        { 0x2E, "Del",        "DELETE" },
        { 0x24, "Home",       "HOME" },
        { 0x23, "End",        "END" },
        { 0x21, "PageUp",     "PAGE UP" },
        { 0x21, "PgUp",       "PAGE UP" },
        { 0x22, "PageDown",   "PAGE DOWN" },
        { 0x22, "PgDn",       "PAGE DOWN" },
        { 0x5D, "Apps",       "MENU KEY" },       // the context-menu key beside the right Ctrl
        { 0x08, "Backspace",  "BACKSPACE" },
        { 0x14, "CapsLock",   "CAPS LOCK" },
    };
    Count = (int)(sizeof(Names) / sizeof(Names[0]));
    return Names;
}

// Why a key cannot open the menu, or null when it can. The arrows and Esc walk and close the menu itself; Num Lock is
// the renderer's own FSR key; a modifier is half of every combination the game and Windows use; Windows sends no key
// press at all for Print Screen, so a menu on it would never open; and below Backspace there are only mouse buttons.
inline const char* MenuKeyRefusal(int Vk)
{
    if (Vk < 0x08 || Vk > 0xFE)
        return "not a keyboard key";
    if (Vk == 0x1B)
        return "Esc closes the menu";
    if (Vk >= 0x25 && Vk <= 0x28)
        return "the arrows walk the menu";
    if (Vk == 0x90)
        return "Num Lock steps the FSR modes";
    if (Vk == 0x2C)
        return "Windows sends no key press for Print Screen";
    if ((Vk >= 0x10 && Vk <= 0x12) || (Vk >= 0xA0 && Vk <= 0xA5) || Vk == 0x5B || Vk == 0x5C)
        return "a modifier key - the game and Windows need it in combinations";
    return nullptr;
}

enum EMenuKeyParse
{
    MenuKey_Ok = 0,
    MenuKey_Empty,        // no value: the default stands, and there is nothing to say about it
    MenuKey_Unknown,      // not a name, a letter, a digit or a number
    MenuKey_Refused       // a key, but one MenuKeyRefusal turns down - Vk says which
};

// Reads a DebugKey value into a virtual-key code. Case, spaces, '_' and '-' do not count, and neither do doubled
// letters - so ScrollLock, "scroll lock" and scrollock all name the same key: its double L is the most misspelt thing
// about it. Vk is the default unless the result is MenuKey_Ok, except that a refused key is left in it for the log.
template <typename TChar>
inline EMenuKeyParse ParseMenuKey(const TChar* Text, int& Vk)
{
    Vk = MenuKeyDefault;
    if (!Text)
        return MenuKey_Empty;
    // Lower case, without the separators and the trimming blanks.
    char S[40];
    int  n = 0;
    for (int i = 0; Text[i] && n < 39; i++)
    {
        unsigned C = (unsigned)Text[i];
        if (C == ' ' || C == '\t' || C == '_' || C == '-')
            continue;
        if (C >= 'A' && C <= 'Z')
            C += 'a' - 'A';
        if (C > 0x7E)
            return MenuKey_Unknown;
        S[n++] = (char)C;
    }
    S[n] = 0;
    if (n == 0)
        return MenuKey_Empty;

    int Found = -1;
    // A number: 0x-hex, or decimal.
    if (S[0] == '0' && S[1] == 'x' && n > 2)
    {
        int V = 0;
        for (int i = 2; i < n; i++)
        {
            const char C = S[i];
            const int  D = (C >= '0' && C <= '9') ? C - '0' : ((C >= 'a' && C <= 'f') ? C - 'a' + 10 : -1);
            if (D < 0 || V > 0xFFF)
                return MenuKey_Unknown;
            V = V * 16 + D;
        }
        Found = V;
    }
    else if (n > 1 && S[0] >= '0' && S[0] <= '9')
    {
        int V = 0;
        for (int i = 0; i < n; i++)
        {
            if (S[i] < '0' || S[i] > '9' || V > 9999)
                return MenuKey_Unknown;
            V = V * 10 + (S[i] - '0');
        }
        Found = V;
    }
    // One letter or digit: its virtual-key code is its capital.
    else if (n == 1 && ((S[0] >= 'a' && S[0] <= 'z') || (S[0] >= '0' && S[0] <= '9')))
        Found = (S[0] >= 'a') ? S[0] - 'a' + 'A' : S[0];
    // F1 to F24.
    else if (S[0] == 'f' && n >= 2 && n <= 3 && S[1] >= '1' && S[1] <= '9' && (n == 2 || (S[2] >= '0' && S[2] <= '9')))
    {
        const int F = (n == 2) ? S[1] - '0' : (S[1] - '0') * 10 + (S[2] - '0');
        if (F < 1 || F > 24)
            return MenuKey_Unknown;
        Found = 0x70 + F - 1;
    }
    else
    {
        // A name, compared with the doubled letters taken out of both sides.
        char Mine[40];
        int  m = 0;
        for (int i = 0; i < n; i++)
            if (m == 0 || Mine[m - 1] != S[i])
                Mine[m++] = S[i];
        Mine[m] = 0;
        int Count = 0;
        const FMenuKeyName* Names = MenuKeyNames(Count);
        for (int q = 0; q < Count && Found < 0; q++)
        {
            char Theirs[40];
            int  t = 0;
            for (int i = 0; Names[q].Ini[i] && t < 39; i++)
            {
                char C = Names[q].Ini[i];
                if (C >= 'A' && C <= 'Z')
                    C = (char)(C + ('a' - 'A'));
                if (t == 0 || Theirs[t - 1] != C)
                    Theirs[t++] = C;
            }
            Theirs[t] = 0;
            int k = 0;
            while (Mine[k] && Mine[k] == Theirs[k])
                k++;
            if (Mine[k] == 0 && Theirs[k] == 0)
                Found = Names[q].Vk;
        }
        if (Found < 0)
            return MenuKey_Unknown;
    }
    if (MenuKeyRefusal(Found))
    {
        Vk = Found;
        return MenuKey_Refused;
    }
    Vk = Found;
    return MenuKey_Ok;
}

// The key as the screen says it: "SCROLL LOCK", "F10", "K" - and "KEY 186" for one with no name of its own.
template <typename TChar>
inline void MenuKeyCaption(int Vk, TChar* Out, int Max)
{
    char T[24];
    int  n = 0;
    int  Count = 0;
    const FMenuKeyName* Names = MenuKeyNames(Count);
    const char* Named = nullptr;
    for (int q = 0; q < Count && !Named; q++)
        if (Names[q].Vk == Vk)
            Named = Names[q].Caption;
    if (Named)
        for (; Named[n] && n < 23; n++)
            T[n] = Named[n];
    else if (Vk >= 0x70 && Vk <= 0x87)
    {
        const int F = Vk - 0x70 + 1;
        T[n++] = 'F';
        if (F >= 10)
            T[n++] = (char)('0' + F / 10);
        T[n++] = (char)('0' + F % 10);
    }
    else if ((Vk >= 'A' && Vk <= 'Z') || (Vk >= '0' && Vk <= '9'))
        T[n++] = (char)Vk;
    else
    {
        const char* K = "KEY ";
        for (; K[n]; n++)
            T[n] = K[n];
        const int V = (Vk < 0) ? 0 : Vk;
        if (V >= 100)
            T[n++] = (char)('0' + (V / 100) % 10);
        if (V >= 10)
            T[n++] = (char)('0' + (V / 10) % 10);
        T[n++] = (char)('0' + V % 10);
    }
    T[n] = 0;
    int k = 0;
    for (; T[k] && k < Max - 1; k++)
        Out[k] = (TChar)T[k];
    Out[k] = (TChar)0;
}

// The key as the settings file spells it - what a first start writes out, and what ParseMenuKey reads back as the same
// key: the first name a key has, F1 to F24, the letter or digit itself, or 0x and two hex digits.
inline void MenuKeyIniName(int Vk, char* Out, int Max)
{
    char T[24];
    int  n = 0;
    int  Count = 0;
    const FMenuKeyName* Names = MenuKeyNames(Count);
    const char* Named = nullptr;
    for (int q = 0; q < Count && !Named; q++)
        if (Names[q].Vk == Vk)
            Named = Names[q].Ini;
    if (Named)
        for (; Named[n] && n < 23; n++)
            T[n] = Named[n];
    else if (Vk >= 0x70 && Vk <= 0x87)
    {
        const int F = Vk - 0x70 + 1;
        T[n++] = 'F';
        if (F >= 10)
            T[n++] = (char)('0' + F / 10);
        T[n++] = (char)('0' + F % 10);
    }
    else if ((Vk >= 'A' && Vk <= 'Z') || (Vk >= '0' && Vk <= '9'))
        T[n++] = (char)Vk;
    else
    {
        const char* Hex = "0123456789ABCDEF";
        const int   V   = Vk & 0xFF;
        T[n++] = '0';
        T[n++] = 'x';
        T[n++] = Hex[(V >> 4) & 15];
        T[n++] = Hex[V & 15];
    }
    T[n] = 0;
    int k = 0;
    for (; T[k] && k < Max - 1; k++)
        Out[k] = T[k];
    Out[k] = 0;
}

// ---------------------------------------------------------------------------
// Windows display scaling: the exe's compatibility setting
// ---------------------------------------------------------------------------

// Windows keeps an exe's compatibility settings as one string of words, under the exe's full path in
// HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers, and applies them when the exe starts. The
// Compatibility tab's "Override high DPI scaling behavior" writes one of three: HIGHDPIAWARE ("Application": the game
// gets real pixels and Windows scales nothing), DPIUNAWARE ("System") or GDIDPISCALING DPIUNAWARE ("System
// (Enhanced)"). Every other word is another setting of that tab - a Windows version, run as administrator - and stays,
// a leading "~" included; a string with no words becomes the "~ HIGHDPIAWARE" the tab itself writes.
enum ECompatLayers
{
    CompatLayers_Same,      // HIGHDPIAWARE is in and neither of the other two: nothing to write (Out is not for use)
    CompatLayers_Changed,   // Out is the string to write
    CompatLayers_TooLong,   // it would not fit Out; Out is empty
};

// Word is Len characters, not terminated; Name is in capitals.
inline bool CompatWordIs(const char* Word, int Len, const char* Name)
{
    int i = 0;
    for (; i < Len && Name[i]; i++)
    {
        const char C = (Word[i] >= 'a' && Word[i] <= 'z') ? (char)(Word[i] - 'a' + 'A') : Word[i];
        if (C != Name[i])
            return false;
    }
    return i == Len && !Name[i];
}

// Appends one word, a space before it unless it is the first; false when Out has no room for it.
inline bool CompatPutWord(char* Out, int Max, int& n, const char* Word, int Len)
{
    if (n + (n ? 1 : 0) + Len + 1 > Max)
        return false;
    if (n)
        Out[n++] = ' ';
    for (int i = 0; i < Len; i++)
        Out[n++] = Word[i];
    Out[n] = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Frame rate limit
// ---------------------------------------------------------------------------

// The FPSLimit setting: Off, Auto, or a number of frames a second. A number means exactly that number - whoever
// writes one is giving an instruction, and a limit that quietly becomes something else is the kind of surprise that
// makes a setting untrustworthy. The "aim a little below" trick belongs to Auto, where the renderer is the one
// choosing.
enum { FpsLimit_Off = 0, FpsLimit_Auto = -1 };

// Anything unreadable keeps Default. The number is clamped to a range that is still a game: below ten nothing is
// playable, above a thousand no display exists.
inline int ParseFpsLimit(const char* Text, int Default)
{
    if (!Text || !Text[0])
        return Default;
    int i = 0;
    while (Text[i] == ' ' || Text[i] == '\t')
        i++;
    // Whole words only, so that a typo is a typo and not a silent Off: "nonsense" keeps the default.
    int e = i;
    while (Text[e] && Text[e] != ' ' && Text[e] != '\t')
        e++;
    if (CompatWordIs(Text + i, e - i, "OFF") || CompatWordIs(Text + i, e - i, "NO"))
        return FpsLimit_Off;
    if (CompatWordIs(Text + i, e - i, "AUTO") || CompatWordIs(Text + i, e - i, "ON"))
        return FpsLimit_Auto;
    if (Text[i] < '0' || Text[i] > '9')
        return Default;
    int V = 0;
    for (; Text[i] >= '0' && Text[i] <= '9' && V < 100000; i++)
        V = V * 10 + (Text[i] - '0');
    if (V < 10)
        return 10;
    return (V > 1000) ? 1000 : V;
}

// Whether the screen the game is on runs with variable refresh. The answer comes from the graphics driver, which is
// the only thing that knows; NoData means no driver would say, and is treated as On - backing off a few frames costs
// a screen that does not vary its refresh almost nothing, while running into the top of a variable one is felt.
enum EVrr { Vrr_Off = 0, Vrr_On = 1, Vrr_NoData = 2 };

// What actually runs: 0 means no limit. Auto takes the screen's refresh rate - where the refresh varies, three frames
// below it, so the picture stays inside that window instead of teetering at its top edge (60 Hz -> 57, 144 Hz -> 141).
// With no refresh rate to go by, Auto limits nothing.
inline int FpsLimitTarget(int Setting, int RefreshHz, EVrr Vrr)
{
    if (Setting > 0)
        return Setting;
    if (Setting == FpsLimit_Off || RefreshHz <= 0)
        return 0;
    if (Vrr == Vrr_Off)
        return RefreshHz;
    const int Below = RefreshHz - 3;
    return (Below < 30) ? 30 : Below;
}

// The limit's slots (FBackend's present): when the next frame may start, from when this one did (Slot, 0 for none yet),
// when it was presented (Now) and the period, in seconds. A frame done within its period waits out the rest of it, and
// the slots walk forward by whole periods, so a long frame is not made up for by a burst of short ones. A frame that took
// longer than its period has waited enough: the next one starts at once, and the slots walk on from there. Starting the
// count again a whole period on instead makes every late frame wait one period more: frames of about 20 ms under a limit
// of 57 then run at about 31 a second where the card could do 50, and the card, idle half the time, clocks down and
// makes them longer still.
inline double NextFrameSlot(double Slot, double Now, double Period)
{
    if (Slot <= 0.0 || Now > Slot + Period)
        return Now;
    return Slot + Period;
}

// The one case where the player is told anything about it: Windows is scaling the game right now, the compatibility
// setting was NOT there, we wrote it, and reading it back proves it took - so a restart really will fix the picture.
// Every other case stays silent: a setting already in place needs no words; a write that failed is nothing the player
// can act on (no rights, a policy, Wine); and on a monitor that is not scaled at all there is nothing to restart for,
// even though the setting is written for the day there is.
inline bool DpiNoticeWanted(bool bWasAlreadySet, bool bWritten, bool bVerified, bool bScalingNow)
{
    return !bWasAlreadySet && bWritten && bVerified && bScalingNow;
}

inline ECompatLayers CompatLayersForHighDpi(const char* Old, char* Out, int Max)
{
    if (Max <= 0)
        return CompatLayers_TooLong;
    Out[0] = 0;
    int  n        = 0;
    bool bFits    = true;
    bool bAny     = false;
    bool bHas     = false;
    bool bDropped = false;
    for (int i = 0; Old && Old[i];)
    {
        while (Old[i] == ' ' || Old[i] == '\t')
            i++;
        const int s = i;
        while (Old[i] && Old[i] != ' ' && Old[i] != '\t')
            i++;
        const int Len = i - s;
        if (!Len)
            break;
        bAny = true;
        if (CompatWordIs(Old + s, Len, "DPIUNAWARE") || CompatWordIs(Old + s, Len, "GDIDPISCALING"))
        {
            bDropped = true;
            continue;
        }
        bHas  = bHas || CompatWordIs(Old + s, Len, "HIGHDPIAWARE");
        bFits = CompatPutWord(Out, Max, n, Old + s, Len) && bFits;
    }
    if (bHas && !bDropped)
        return CompatLayers_Same;
    if (!bAny)
        bFits = CompatPutWord(Out, Max, n, "~", 1) && bFits;
    if (!bHas)
        bFits = CompatPutWord(Out, Max, n, "HIGHDPIAWARE", 12) && bFits;
    if (!bFits)
    {
        Out[0] = 0;
        return CompatLayers_TooLong;
    }
    return CompatLayers_Changed;
}

// ---------------------------------------------------------------------------------------------------------------
//  The shader cache's file: which of the shaders it holds are written back.
// ---------------------------------------------------------------------------------------------------------------
// The file outlives the build that wrote it, so a shader whose text a newer build changed stays in it with nobody to
// ask for it again. What is written: every shader asked for in this process; of the others, the ones asked for within
// KeepFor of Now - and only while the file stays within MaxBytes: where it would not, the ones asked for longest ago
// are left out first (of two asked for at the same moment, the one earlier in the list). Asked, Now and KeepFor are
// in one unit of time; Size is a shader's own bytes and Head what each costs in the file beside them. Keep gets 1 or
// 0 for each shader; the result is how many are written.
inline int ShaderCacheWritten(const unsigned long long* Asked, const unsigned int* Size, const unsigned char* AskedNow,
                              int Count, unsigned long long Now, unsigned long long KeepFor,
                              unsigned long long MaxBytes, unsigned int Head, unsigned char* Keep)
{
    unsigned long long Total   = 0;
    int                Written = 0;
    for (int i = 0; i < Count; i++)
    {
        // A time ahead of Now is a clock that was set back since: taken for a fresh one.
        const bool bFresh = Asked[i] >= Now || Now - Asked[i] <= KeepFor;
        Keep[i] = (AskedNow[i] || bFresh) ? 1 : 0;
        if (Keep[i])
        {
            Total += (unsigned long long)Head + Size[i];
            Written++;
        }
    }
    while (Total > MaxBytes)
    {
        int Oldest = -1;
        for (int i = 0; i < Count; i++)
            if (Keep[i] && !AskedNow[i] && (Oldest < 0 || Asked[i] < Asked[Oldest]))
                Oldest = i;
        // Only this process's own are left: those are written whatever they come to.
        if (Oldest < 0)
            break;
        Keep[Oldest] = 0;
        Total -= (unsigned long long)Head + Size[Oldest];
        Written--;
    }
    return Written;
}

// The game's loading pictures (RevisionSplashScreenFix). The game draws one pixel for pixel, its middle cut out to the
// screen's size, up to the picture's own size - and on a screen taller than that it draws a black tile in its place.
const float SplashMaxWidth  = 4096.0f;
const float SplashMaxHeight = 2048.0f;

// What such a picture is for is in its middle: the chapter's number and name, a small picture, the word LOADING. The
// rest of it is a blurred backdrop, there to be cut from. This much of the middle holds all of it.
const float SplashContentWidth  = 1280.0f;
const float SplashContentHeight = 720.0f;

// Such a picture laid on a screen: in the screen's middle, at the size at which the picture's middle
// (SplashContentWidth by SplashContentHeight) just fits the screen. On a screen of that shape the middle fills it. On
// a taller one its width is the screen's, and more of the backdrop shows above and below it; on a wider one its height
// is the screen's, and more shows at its sides - as far as the picture goes, and the screen is black beyond it. Gives
// the part of the picture that is seen (U, V, UL, VL, in the picture's own pixels) and where it stands on the screen
// (X, Y, W, H). False for a picture or a screen without a size.
inline bool SplashView(float ScreenW, float ScreenH, float PictureW, float PictureH,
                       float& U, float& V, float& UL, float& VL, float& X, float& Y, float& W, float& H)
{
    if (!(ScreenW >= 1.0f && ScreenH >= 1.0f && PictureW >= 1.0f && PictureH >= 1.0f))
        return false;
    const float ContentW = (PictureW < SplashContentWidth) ? PictureW : SplashContentWidth;
    const float ContentH = (PictureH < SplashContentHeight) ? PictureH : SplashContentHeight;
    const float Wide     = ScreenW / ContentW;
    const float High     = ScreenH / ContentH;
    const float Scale    = (Wide < High) ? Wide : High;      // the screen's pixels to one of the picture's
    UL = (ScreenW / Scale < PictureW) ? ScreenW / Scale : PictureW;
    VL = (ScreenH / Scale < PictureH) ? ScreenH / Scale : PictureH;
    U  = (PictureW - UL) * 0.5f;
    V  = (PictureH - VL) * 0.5f;
    W  = UL * Scale;
    H  = VL * Scale;
    X  = (ScreenW - W) * 0.5f;
    Y  = (ScreenH - H) * 0.5f;
    return true;
}

// ---------------------------------------------------------------------------
// Grass (Grass3D) and the wind - docs/terv-novenyzet-szel.md
// ---------------------------------------------------------------------------
//
// The blades grow on the level's own grass: the surfaces whose texture is one of the game's grass textures, where they
// face up. Nothing of them is kept. The world is cut into tiles of a fixed grid, every tile a square of cells, and every
// cell has one root at a place its own number decides - so a blade stands where it stood, frame after frame, whatever
// the eye does, and a root further off is kept or left by that number too, never by where the tile happens to start.
// The vertex shader makes the blades (GrassShader.h); what it shares with this side is said here, once.

enum
{
    GrassTileCells     = 8,                                   // a tile is this many cells a side
    GrassTileRoots     = GrassTileCells * GrassTileCells,
    GrassBladesPerRoot = 4,                                   // a clump: the blades of a root lean together
    GrassTuftBlades    = 96,                                  // a tuft's (the level's foliage sheets grown again)
    GrassTuftRows      = 2,                                   // a tuft's float4 rows: foot and height, its reach
    GrassTileTris      = 4,                                   // a tile's ground: at most this many of the triangles
    GrassTileRows      = 1 + 3 * GrassTileTris,               // a tile's float4 rows in the shader's block
    GrassHeadRows      = 16,                                  // the block's rows before the pushes
    GrassPushMax       = 32,                                  // what stands on the grass, nearest first
    GrassBlockRows     = 4096,                                // a constant buffer's most
    GrassTilesPerBlock = (GrassBlockRows - GrassHeadRows - GrassPushMax) / GrassTileRows,
    GrassBladeVerts    = 7,                                   // a strip: left and right at three steps, the tip
    GrassPictureWidth  = 64,                                  // the blades' colours: root to tip along
    GrassPictureRows   = 32                                   // and their kinds down - three clumps', the two
                                                              // tufts' - four rows each (GrassShader.h)
};
static const float GrassCell   = 2.75f;                       // a cell's side, level units (5 cm)
static const float GrassTile   = GrassCell * (float)GrassTileCells;
static const float GrassNear   = 300.0f;                      // every root this close (6 m)
static const float GrassFar    = 1400.0f;                     // none beyond (28 m)
static const float GrassUpMin  = 0.5f;                        // the least Z of a ground triangle's normal

// A blade's own shade (GrassShader.h, VSGrass): a blade takes less of the ground's light than the ground does, the least
// at its root and the most at its tip - the blades around it shut out that much of the light from above, more of it
// lower down. A fixed shade of the blade's own, the same in every frame, so that it cannot flicker: worked out from the
// depth instead, it turned from frame to frame on blades thinner than a pixel as the picture's jitter moved. Macros, for
// the shader's text (RDX_STRINGIFY).
#define RDX_GRASS_SHADE_ROOT 0.30f
#define RDX_GRASS_SHADE_TIP  0.62f
static const float GrassShadeRoot = RDX_GRASS_SHADE_ROOT;
static const float GrassShadeTip  = RDX_GRASS_SHADE_TIP;

// The share of a distance's roots that grow: all near, then falling with the distance squared - so a ring of the
// ground holds blades in proportion to the log of its radius, not the radius - and nothing past GrassFar.
inline float GrassKeep(float Dist)
{
    if (Dist <= GrassNear)
        return 1.0f;
    if (Dist >= GrassFar)
        return 0.0f;
    const float R = GrassNear / Dist;
    const float F = (GrassFar - Dist) / (GrassFar * 0.25f);   // the last quarter fades to nothing
    return R * R * ((F < 1.0f) ? F : 1.0f);
}

// The grass's levels (Grass3D): Off, or Low - the grass as it was made - Medium and High. A level stretches the keep
// rule by its reach - GrassKeep is asked at the distance over it, so the blades thin out that much further off and are
// gone that much further - and gives a root its own number of blades: Medium a little denser and half as far again,
// High twice as far and the densest - two clumps to a root's cell (GrassClumpsOf) - near the eye, thinning off to Low's
// where a blade would be under a pixel wide (GrassThinFarOf). The ini key takes True (Low) and False (Off) too.
enum EGrass
{
    Grass_Off = 0,
    Grass_Low,
    Grass_Medium,
    Grass_High,
    Grass_Count
};
inline const char* const* GrassNames()
{
    static const char* const Names[Grass_Count] = { "Off", "Low", "Medium", "High" };
    return Names;
}
inline float GrassReachOf(EGrass L)
{
    static const float Reach[Grass_Count] = { 0.0f, 1.0f, 1.5f, 2.0f };
    return Reach[(L >= 0 && L < Grass_Count) ? (int)L : 0];
}
inline int GrassBladesOf(EGrass L)
{
    static const int Blades[Grass_Count] = { 0, GrassBladesPerRoot, GrassBladesPerRoot + 1, 2 * (GrassBladesPerRoot + 2) };
    return Blades[(L >= 0 && L < Grass_Count) ? (int)L : 0];
}
// The clumps a root's blades are shared among, each in its own place in the root's cell: one, High's two.
inline int GrassClumpsOf(EGrass L)
{
    return (L == Grass_High) ? 2 : 1;
}
// How much nearer a root's blades beyond Low's - its further clumps, and the more blades of its first - thin out than
// the root itself (GrassShader.h, GSpare[1].x): High's 2.5, so that near the eye its grass is all of its own and off
// where a blade would be under a pixel wide only Low's - grown in again as the eye comes nearer; none for the others.
inline float GrassThinFarOf(EGrass L)
{
    return (L == Grass_High) ? 2.5f : 1.0f;
}
template <typename TChar>
inline void GrassName(EGrass L, TChar* Out, int Max)
{
    const char* const N = GrassNames()[(L >= 0 && L < Grass_Count) ? (int)L : 0];
    int k = 0;
    for (; N[k] && k < Max - 1; k++)
        Out[k] = (TChar)N[k];
    Out[k] = (TChar)0;
}
// Low for anything it does not recognise, with bKnown false so the caller can say so in the log.
template <typename TChar>
inline EGrass ParseGrass(const TChar* Text, bool& bKnown)
{
    bKnown = false;
    if (!Text)
        return Grass_Low;
    int B = 0;
    while (Text[B] == ' ' || Text[B] == '\t')
        B++;
    for (int i = 0; i < Grass_Count; i++)
    {
        const char* const N = GrassNames()[i];
        int k = 0;
        while (N[k] && Text[B + k])
        {
            const TChar a = Text[B + k];
            const char  b = N[k];
            const TChar la = (a >= 'A' && a <= 'Z') ? (TChar)(a + 32) : a;
            const char  lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != (TChar)lb)
                break;
            k++;
        }
        if (!N[k] && (!Text[B + k] || Text[B + k] == ' ' || Text[B + k] == '\t' || Text[B + k] == '\r'))
        {
            bKnown = true;
            return (EGrass)i;
        }
    }
    return Grass_Low;
}
inline EGrass GrassStep(EGrass L, int Step)
{
    int N = (int)L + Step;
    while (N < 0)
        N += Grass_Count;
    return (EGrass)(N % Grass_Count);
}

// Which of the game's grass textures a texture's full name names, in any case: 0 none, 1 the short grass (5 to 10 cm),
// 2 the long (10 to 20 cm). Four of the game's five (docs/terv-novenyzet-szel.md 2.1): the lawns' are long, Battery
// Park's and the darker patches' short. Not Battery Park's NgrassB: most of its picture is a patch of bare earth and
// stones, the grass only round it - blades over all of it would grow out of the earth.
template <typename TChar>
inline int GrassKindOf(const TChar* Name)
{
    if (!Name)
        return 0;
    static const char* const Long[]  = { "clengrengrass_a", "litegrengrass_a" };
    static const char* const Short[] = { "darkgrengrass_a", "ngrass" };
    // The name's last part, after its last dot: Package.Group.Name.
    int Start = 0;
    for (int i = 0; Name[i]; i++)
        if (Name[i] == '.')
            Start = i + 1;
    for (int List = 0; List < 2; List++)
    {
        const char* const* Names = List ? Short : Long;
        const int          Count = 2;
        for (int n = 0; n < Count; n++)
        {
            int k = 0;
            for (; Names[n][k]; k++)
            {
                TChar C = Name[Start + k];
                if (C >= 'A' && C <= 'Z')
                    C = (TChar)(C + ('a' - 'A'));
                if (C != (TChar)Names[n][k])
                    break;
            }
            if (!Names[n][k] && !Name[Start + k])
                return List ? 1 : 2;
        }
    }
    return 0;
}

// A turn's sine - X in turns, not radians - without a library, close to a thousandth: the wind needs its shape, not
// its last digit. The shader uses its own sin(), which is closer still.
inline float SinTurns(float X)
{
    X = X - (float)(int)X;                    // to -1..1
    if (X > 0.5f)
        X -= 1.0f;
    if (X < -0.5f)
        X += 1.0f;
    // Bhaskara's form, on -0.5..0.5 turns, with the sign carried.
    const float A = (X < 0.0f) ? -X : X;
    const float S = 16.0f * A * (0.5f - A) / (1.25f - 4.0f * A * (0.5f - A));
    return (X < 0.0f) ? -S : S;
}

// The wind (docs/terv-novenyzet-szel.md 3.1): a direction the level keeps, a base strength, and gusts that travel along
// the direction - their fronts move downwind - in two waves of lengths of their own, so they do not repeat in step.
// Strengths in a blade's height: how far its tip is pushed. The shader's WindAt is the same formula.
struct FWind
{
    float DirX, DirY;       // where it blows, a unit vector in the world's X and Y
    float Base;             // its strength between gusts
    float Gust;             // a gust's strength on top
    float Wave;             // a gust's length along the wind, level units
    float Speed;            // how fast the gusts travel, level units a second
};

inline void WindAt(const FWind& W, float X, float Y, float T, float& OutX, float& OutY)
{
    const float Along  = X * W.DirX + Y * W.DirY;
    const float Across = -X * W.DirY + Y * W.DirX;
    const float Wave   = (W.Wave > 1.0f) ? W.Wave : 1.0f;
    const float P1     = (Along - W.Speed * T) / Wave;
    const float P2     = (Along * 0.61f - W.Speed * 0.83f * T) / (Wave * 0.37f) + Across / (Wave * 0.9f);
    float G = 0.5f + 0.5f * SinTurns(P1) * (0.6f + 0.4f * SinTurns(P2 + 0.27f));
    G = G * G;              // a gust is short: most of the time the wind is at its base
    const float S = W.Base + W.Gust * G;
    OutX = W.DirX * S;
    OutY = W.DirY * S;
}

// A 32-bit hash, the shader's GrassHash too: what a cell's root, a blade's height, lean and colour are drawn from.
inline unsigned int GrassHash(unsigned int X)
{
    X ^= X >> 16;
    X *= 0x7feb352dU;
    X ^= X >> 15;
    X *= 0x846ca68bU;
    X ^= X >> 16;
    return X;
}

// The level's wind: its direction from the level's name - the same on every visit - and the strengths Laci asked for,
// "very weak to slightly weak": a base a twelfth of a blade's height, gusts to a third.
inline FWind WindForLevel(unsigned int NameHash)
{
    FWind W;
    const float Turn = (float)(GrassHash(NameHash) & 0xFFFFu) / 65536.0f;
    W.DirX  = SinTurns(Turn + 0.25f);
    W.DirY  = SinTurns(Turn);
    const float L = Sqrt(W.DirX * W.DirX + W.DirY * W.DirY);
    W.DirX  = (L > 0.0f) ? W.DirX / L : 1.0f;
    W.DirY  = (L > 0.0f) ? W.DirY / L : 0.0f;
    W.Base  = 0.08f;
    W.Gust  = 0.25f;
    W.Wave  = 900.0f;
    W.Speed = 260.0f;
    return W;
}

// How strong the wind is at the level's time T, a share of the level's own (WindForLevel) that the base and the gusts
// are both taken at: it comes and goes slowly - at times all but still, never quite (WindCalmest), at others up to the
// level's own - and the gusts ride on it (WindAt). Three slow waves of lengths of their own, so the calms and the blows
// do not come round in step; their phases from the level's name. Shaped to stay a while near either end rather than
// pass straight through the middle, and to rise and fall a little on its way.
static const float WindCalmest = 0.08f;
inline float WindStrength(float T, unsigned int NameHash)
{
    const unsigned int H  = GrassHash(NameHash ^ 0x9E3779B9u);
    const float        P1 = (float)(H & 0x3FFu) / 1024.0f;
    const float        P2 = (float)((H >> 10) & 0x3FFu) / 1024.0f;
    const float        P3 = (float)((H >> 20) & 0x3FFu) / 1024.0f;
    const float Sum = 0.55f * SinTurns(T / 67.0f + P1) + 0.30f * SinTurns(T / 41.0f + P2) +
                      0.15f * SinTurns(T / 23.0f + P3);
    float X = 0.5f + 0.5f * Sum;
    X = (X < 0.0f) ? 0.0f : ((X > 1.0f) ? 1.0f : X);
    X = X * X * (3.0f - 2.0f * X);
    return WindCalmest + (1.0f - WindCalmest) * X;
}

// The tiles of the world's grid a footprint from (MinX, MinY) to (MaxX, MaxY) touches, in whole tiles, both ends kept.
inline void GrassTileSpan(float MinX, float MinY, float MaxX, float MaxY, int& X0, int& Y0, int& X1, int& Y1)
{
    const float Inv = 1.0f / GrassTile;
    const float Fx0 = MinX * Inv, Fy0 = MinY * Inv, Fx1 = MaxX * Inv, Fy1 = MaxY * Inv;
    X0 = (int)Fx0 - ((Fx0 < 0.0f && (float)(int)Fx0 != Fx0) ? 1 : 0);
    Y0 = (int)Fy0 - ((Fy0 < 0.0f && (float)(int)Fy0 != Fy0) ? 1 : 0);
    X1 = (int)Fx1 - ((Fx1 < 0.0f && (float)(int)Fx1 != Fx1) ? 1 : 0);
    Y1 = (int)Fy1 - ((Fy1 < 0.0f && (float)(int)Fy1 != Fy1) ? 1 : 0);
}

// A grass pair (RenderDevice.cpp, GrowGrass: a tile, a triangle that reaches into it, and the tile's thinning band) as
// one number in the pairs' order - the band, then the tile's row, its column, then the triangle - from the highest bits
// down. False where one of them does not fit: a band past 15, a tile index outside sixteen bits round nought, a
// triangle past 28 bits.
inline bool GrassPairKey(int Band, int X, int Y, int Tri, unsigned long long& Key)
{
    if (Band < 0 || Band > 15 || X < -32768 || X > 32767 || Y < -32768 || Y > 32767 || Tri < 0 || Tri >= (1 << 28))
        return false;
    Key = ((unsigned long long)Band << 60) | ((unsigned long long)(Y + 32768) << 44) |
          ((unsigned long long)(X + 32768) << 28) | (unsigned long long)Tri;
    return true;
}

inline void GrassPairOfKey(unsigned long long Key, int& Band, int& X, int& Y, int& Tri)
{
    Band = (int)(Key >> 60);
    Y    = (int)((Key >> 44) & 0xFFFFu) - 32768;
    X    = (int)((Key >> 28) & 0xFFFFu) - 32768;
    Tri  = (int)(Key & 0xFFFFFFFu);
}

// Count numbers put in order a byte at a time, the lowest byte first (a radix sort): A holds them, B is as much room
// again for each pass to write into, and a byte they all share is passed over. Where they end up: A or B.
inline unsigned long long* SortByBytes(unsigned long long* A, unsigned long long* B, int Count)
{
    for (int Shift = 0; Shift < 64 && Count > 1; Shift += 8)
    {
        int Place[256] = {};
        for (int i = 0; i < Count; i++)
            Place[(A[i] >> Shift) & 255u]++;
        if (Place[(A[0] >> Shift) & 255u] == Count)
            continue;
        for (int d = 0, Sum = 0; d < 256; d++)
        {
            const int c = Place[d];
            Place[d]    = Sum;
            Sum += c;
        }
        for (int i = 0; i < Count; i++)
            B[Place[(A[i] >> Shift) & 255u]++] = A[i];
        unsigned long long* const T = A;
        A = B;
        B = T;
    }
    return A;
}

// A triangle of the ground as the shader takes it: Row[0..5] its corners' X and Y; Row[6..8] the plane its Z is read
// from, Z = Row[8] + Row[6] X + Row[7] Y. False for one too steep for grass to stand on (its normal's Z, either way up,
// under GrassUpMin) or with no area.
inline bool GrassTriangle(const FVec3& A, const FVec3& B, const FVec3& C, float* Row)
{
    const FVec3 E1 = { B.X - A.X, B.Y - A.Y, B.Z - A.Z };
    const FVec3 E2 = { C.X - A.X, C.Y - A.Y, C.Z - A.Z };
    const FVec3 N  = { E1.Y * E2.Z - E1.Z * E2.Y, E1.Z * E2.X - E1.X * E2.Z, E1.X * E2.Y - E1.Y * E2.X };
    const float L  = Sqrt(Dot(N, N));
    if (!(L > 1e-6f))
        return false;
    const float Nz = N.Z / L;
    if (Nz < GrassUpMin && Nz > -GrassUpMin)
        return false;
    Row[0] = A.X;
    Row[1] = A.Y;
    Row[2] = B.X;
    Row[3] = B.Y;
    Row[4] = C.X;
    Row[5] = C.Y;
    Row[6] = -N.X / N.Z;
    Row[7] = -N.Y / N.Z;
    Row[8] = A.Z - Row[6] * A.X - Row[7] * A.Y;
    return true;
}

// Whether (X, Y) lies in such a triangle - its edges kept - as the shader tests a root.
inline bool GrassInside(const float* Row, float X, float Y)
{
    const float D1 = (Row[2] - Row[0]) * (Y - Row[1]) - (Row[3] - Row[1]) * (X - Row[0]);
    const float D2 = (Row[4] - Row[2]) * (Y - Row[3]) - (Row[5] - Row[3]) * (X - Row[2]);
    const float D3 = (Row[0] - Row[4]) * (Y - Row[5]) - (Row[1] - Row[5]) * (X - Row[4]);
    const bool  Neg = (D1 < 0.0f) || (D2 < 0.0f) || (D3 < 0.0f);
    const bool  Pos = (D1 > 0.0f) || (D2 > 0.0f) || (D3 > 0.0f);
    return !(Neg && Pos);
}

// Whether such a triangle reaches into the box from (X0, Y0) to (X1, Y1), edges kept: its footprint overlaps the box,
// and the box does not lie wholly past one of its edges - which together is exact for a triangle and a box.
// The ways the game's trees and bushes grow again (Trees3D): none, EZ-Tree's, the renderer's own, half of each.
enum { Trees_Off = 0, Trees_Ez = 1, Trees_Own = 2, Trees_Mixed = 3 };

// Which of a key's words (lower case, Names) the value is, in any case and with blanks before it: its index, -1 none.
inline int WordOf(const char* V, const char* const* Names, int Count)
{
    if (!V)
        return -1;
    int b = 0;
    while (V[b] == ' ' || V[b] == '\t')
        b++;
    for (int i = 0; i < Count; i++)
    {
        int k = 0;
        while (Names[i][k] && V[b + k] && ((V[b + k] >= 'A' && V[b + k] <= 'Z') ? V[b + k] + 32 : V[b + k]) == Names[i][k])
            k++;
        if (!Names[i][k] && (!V[b + k] || V[b + k] == ' ' || V[b + k] == '\t' || V[b + k] == '\r'))
            return i;
    }
    return -1;
}

// Original (the game's own) or Fixed (the renderer's) - the Fog and Flashlight keys: 1 Fixed, 0 Original, -1 neither.
inline int OriginalOrFixed(const char* V)
{
    static const char* const Names[2] = { "original", "fixed" };
    return WordOf(V, Names, 2);
}

// On or Off - the AntiLag key: 1 On, 0 Off, -1 neither.
inline int OnOrOff(const char* V)
{
    static const char* const Names[2] = { "off", "on" };
    return WordOf(V, Names, 2);
}

// Everything a preset holds: picked in the menu it puts every one of these on, and a start under it runs them,
// whatever the settings file's keys say. Custom holds none - it is only the name for a picture that is none of the
// presets. The game's three fixes, the readout and the views belong to no preset.
struct FPresetPicture
{
    EAaChoice         Aa;
    EUpscaling        Upscale;
    bool              bShadows;
    EShadowQuality    ShadowQ;
    EHdrMode          Hdr;
    EAmbientOcclusion Ao;
    EWater            Water;
    EReflections      Reflections;
    int               FpsLimit;
    bool              bVsync, bFrameGen;
    EGeometry         Geometry;
    bool              bSoft, bVolumetric, bBlur, bRays, bAberr, bVign, bGrain;
    EBloom            Bloom;
    ELensFlare        Flare;
    bool              bDof, bEndless, bFire, bFogRooms;
    EFlashlight       Flashlight;
    EForceField       ForceField;
    EGrass            Grass;
    int               Trees;
    bool              bWind;
    bool              bAntiLag;
};

// The presets' pictures (Laci's table). Vanilla: the game as it shipped, every extra off and each fix of ours the game's
// own way. Recommended: the picture that runs everywhere - High shadows, the SSAO, ten-bit SDR, High water and its
// reflections, the relief by POM, the soft particles, the 3D smoke and fire, the depth of field, the endless water,
// the fixed fog and flashlight, the 3D force fields, the grass at High, the trees both ways and the wind.
// INeverAskedForThis: the same with everything the rays can do - the shadows, the occlusion, the reflections - HDR on
// and the relief raised by displacement mapping. The latency's driver help (AntiLag) is off in all three. Custom has no
// picture: asked, it answers with Recommended's.
inline FPresetPicture PresetPicture(EPreset P)
{
    const bool     bVanilla = (P == Preset_Vanilla), bRays = (P == Preset_INeverAskedForThis);
    FPresetPicture S;
    S.Aa              = bVanilla ? AaChoice_Off : AaChoice_Auto;
    S.Upscale         = bVanilla ? Upscaling_Off : Upscaling_NativeAA;
    S.bShadows        = !bVanilla;
    S.ShadowQ         = bRays ? ShadowQuality_Rt : ShadowQuality_High;
    S.Hdr             = bVanilla ? Hdr_Off : (bRays ? Hdr_On : Hdr_Ten);
    S.Ao              = bVanilla ? Ao_Off : (bRays ? Ao_Rt : Ao_Ssao);
    S.Water           = bVanilla ? Water_Off : Water_High;
    S.Reflections     = bVanilla ? Reflections_Default : (bRays ? Reflections_RtHalf : Reflections_High);
    S.FpsLimit        = bVanilla ? (int)FpsLimit_Off : (int)FpsLimit_Auto;
    S.bVsync          = false;
    S.bFrameGen       = false;
    S.Geometry        = bVanilla ? Geometry_Off : (bRays ? Geometry_Tess : Geometry_Pom);
    S.bSoft           = !bVanilla;
    S.bVolumetric     = !bVanilla;
    S.bBlur           = false;
    S.bRays           = false;
    S.bAberr          = false;
    S.bVign           = false;
    S.bGrain          = false;
    S.Bloom           = Bloom_Off;
    S.Flare           = LensFlare_Off;
    S.bDof            = !bVanilla;
    S.bEndless        = !bVanilla;
    S.bFire           = !bVanilla;
    S.bFogRooms       = !bVanilla;
    S.Flashlight      = bVanilla ? Flashlight_Original : (bRays ? Flashlight_Everybody : Flashlight_Fixed);
    S.ForceField      = bVanilla ? ForceField_Engine : ForceField_Volume;
    S.Grass           = bVanilla ? Grass_Off : Grass_High;
    S.Trees           = bVanilla ? Trees_Off : Trees_Mixed;
    S.bWind           = !bVanilla;
    S.bAntiLag        = false;
    return S;
}

// Whether two of a level's plant sheets stand on one foot, and so are one plant's: the middles of their bottom edges
// (Ax, Ay), (Bx, By) within a quarter of the shorter sheet's length across the ground (LenA, LenB). The sheets of one
// plant all start there - crossed at their middles, a star of them, or leaning out from it, a piece of it set higher
// over it; a row's plants stand each on a foot of its own, however their sheets overlap.
inline bool SheetsShareFoot(float Ax, float Ay, float LenA, float Bx, float By, float LenB)
{
    const float Near = 0.25f * ((LenA < LenB) ? LenA : LenB);
    return (Ax - Bx) * (Ax - Bx) + (Ay - By) * (Ay - By) <= Near * Near;
}

inline bool GrassTriangleTouchesBox(const float* Row, float X0, float Y0, float X1, float Y1)
{
    float MinX = Row[0], MaxX = Row[0], MinY = Row[1], MaxY = Row[1];
    for (int k = 1; k < 3; k++)
    {
        MinX = (Row[2 * k] < MinX) ? Row[2 * k] : MinX;
        MaxX = (Row[2 * k] > MaxX) ? Row[2 * k] : MaxX;
        MinY = (Row[2 * k + 1] < MinY) ? Row[2 * k + 1] : MinY;
        MaxY = (Row[2 * k + 1] > MaxY) ? Row[2 * k + 1] : MaxY;
    }
    if (MaxX < X0 || MinX > X1 || MaxY < Y0 || MinY > Y1)
        return false;
    const float Cx[4] = { X0, X1, X0, X1 };
    const float Cy[4] = { Y0, Y0, Y1, Y1 };
    for (int e = 0; e < 3; e++)
    {
        const float Ax = Row[2 * e], Ay = Row[2 * e + 1];
        const float Bx = Row[2 * ((e + 1) % 3)], By = Row[2 * ((e + 1) % 3) + 1];
        const float Ox = Row[2 * ((e + 2) % 3)], Oy = Row[2 * ((e + 2) % 3) + 1];
        const float Ex = Bx - Ax, Ey = By - Ay;
        // The side of the edge the triangle is on: its third corner's.
        const float Side = Ex * (Oy - Ay) - Ey * (Ox - Ax);
        if (Side == 0.0f)
            return false;
        bool bOn = false;
        for (int c = 0; c < 4 && !bOn; c++)
            bOn = (Ex * (Cy[c] - Ay) - Ey * (Cx[c] - Ax)) * Side >= 0.0f;
        if (!bOn)
            return false;
    }
    return true;
}

// The tiles a ground triangle (GrassTriangle's Row) reaches into, worked out once a level so that a frame goes through
// those alone. GrassTriangleSpan: the tiles of its whole footprint, X0..X1 by Y0..Y1 (GrassTileSpan).
// GrassTriangleRows: for each row y of that span, Out[2 (y - Y0)] and Out[2 (y - Y0) + 1] - its first and its last
// column whose tile GrassTriangleTouchesBox finds touched, with the same corners a frame tries it with; the first past
// the last for a row it does not touch. False where a row's touched tiles are not one run, or a column does not fit in a
// short: then a frame tries each tile itself.
inline void GrassTriangleSpan(const float* Row, int& X0, int& Y0, int& X1, int& Y1)
{
    float MinX = Row[0], MaxX = Row[0], MinY = Row[1], MaxY = Row[1];
    for (int k = 1; k < 3; k++)
    {
        MinX = (Row[2 * k] < MinX) ? Row[2 * k] : MinX;
        MaxX = (Row[2 * k] > MaxX) ? Row[2 * k] : MaxX;
        MinY = (Row[2 * k + 1] < MinY) ? Row[2 * k + 1] : MinY;
        MaxY = (Row[2 * k + 1] > MaxY) ? Row[2 * k + 1] : MaxY;
    }
    GrassTileSpan(MinX, MinY, MaxX, MaxY, X0, Y0, X1, Y1);
}

inline bool GrassTriangleRows(const float* Row, int X0, int Y0, int X1, int Y1, short* Out)
{
    if (X0 < -32767 || X1 > 32766)
        return false;
    for (int y = Y0; y <= Y1; y++)
    {
        int  First = X1 + 1, Last = X1;
        bool bRun  = true;
        for (int x = X0; x <= X1; x++)
        {
            const float Lx = (float)x * GrassTile, Ly = (float)y * GrassTile;
            if (!GrassTriangleTouchesBox(Row, Lx, Ly, Lx + GrassTile, Ly + GrassTile))
                continue;
            if (First > X1)
                First = Last = x;
            else if (x == Last + 1)
                Last = x;
            else
                bRun = false;
        }
        if (!bRun)
            return false;
        Out[2 * (y - Y0)]     = (short)First;
        Out[2 * (y - Y0) + 1] = (short)((First > X1) ? X1 : Last);
    }
    return true;
}

} // namespace RdxMath

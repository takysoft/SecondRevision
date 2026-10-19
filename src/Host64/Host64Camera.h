// SecondRevisionHost64 - a frame's camera as matrices, for the backends that ask for them (DLSS's frame generation).
//
// The protocol carries the camera as the renderer has it: where it stands, its three axes, the vertical field of view
// and the near plane (Host64Protocol.h). A backend that wants matrices gets them from these, the same way for every
// frame, so the conventions are in one place:
//   - the camera's space: x to its right, y up, z ahead;
//   - clip space as Direct3D has it: x right, y up, and the protocol's depth - 1 at the near plane down to 0 at an
//     infinite far one, so z/w is Near / z;
//   - four by four, rows first, for row vectors: a point goes from one space to the next as v * M, so A * B is "A,
//     then B".
// No jitter in any of them. Nothing here needs a device or a library: the unit tests hold it to its own algebra.
#pragma once

#include <math.h>

#include "Host64Protocol.h"

namespace Host64
{
    struct FMat
    {
        float M[4][4];
    };

    inline FMat MatMul(const FMat& A, const FMat& B)
    {
        FMat R;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                float S = 0.0f;
                for (int k = 0; k < 4; k++)
                    S += A.M[i][k] * B.M[k][j];
                R.M[i][j] = S;
            }
        return R;
    }

    inline FMat MatTransposed(const FMat& A)
    {
        FMat R;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                R.M[i][j] = A.M[j][i];
        return R;
    }

    inline FMat MatIdentity()
    {
        const FMat R = { { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f },
                           { 0.0f, 0.0f, 0.0f, 1.0f } } };
        return R;
    }

    inline float CameraDot(const float A[3], const float B[3])
    {
        return A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
    }

    // The level's space into the camera's, and back. The camera's axes are unit vectors at right angles, so the way
    // back is the same axes laid the other way, and the camera's place.
    inline FMat WorldToView(const FFrame& F)
    {
        const FMat R = { { { F.CameraRight[0], F.CameraUp[0], F.CameraForward[0], 0.0f },
                           { F.CameraRight[1], F.CameraUp[1], F.CameraForward[1], 0.0f },
                           { F.CameraRight[2], F.CameraUp[2], F.CameraForward[2], 0.0f },
                           { -CameraDot(F.CameraPosition, F.CameraRight), -CameraDot(F.CameraPosition, F.CameraUp),
                             -CameraDot(F.CameraPosition, F.CameraForward), 1.0f } } };
        return R;
    }

    inline FMat ViewToWorld(const FFrame& F)
    {
        const FMat R = { { { F.CameraRight[0], F.CameraRight[1], F.CameraRight[2], 0.0f },
                           { F.CameraUp[0], F.CameraUp[1], F.CameraUp[2], 0.0f },
                           { F.CameraForward[0], F.CameraForward[1], F.CameraForward[2], 0.0f },
                           { F.CameraPosition[0], F.CameraPosition[1], F.CameraPosition[2], 1.0f } } };
        return R;
    }

    // The near plane and the picture's shape, with something sane where a frame left them out.
    inline float CameraNear(const FFrame& F)
    {
        return (F.NearZ > 0.0f) ? F.NearZ : 1.0f;
    }

    inline float CameraAspect(const FFrame& F)
    {
        return (F.RenderWidth && F.RenderHeight) ? (float)F.RenderWidth / (float)F.RenderHeight : 1.0f;
    }

    // The camera's space into clip space, and back.
    inline FMat ViewToClip(const FFrame& F)
    {
        const float Ys = 1.0f / tanf(((F.FovY > 0.0f) ? F.FovY : 1.0f) * 0.5f);
        const float Xs = Ys / CameraAspect(F);
        const FMat  R = { { { Xs, 0.0f, 0.0f, 0.0f }, { 0.0f, Ys, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f },
                            { 0.0f, 0.0f, CameraNear(F), 0.0f } } };
        return R;
    }

    inline FMat ClipToView(const FFrame& F)
    {
        const float Ys = 1.0f / tanf(((F.FovY > 0.0f) ? F.FovY : 1.0f) * 0.5f);
        const float Xs = Ys / CameraAspect(F);
        const FMat  R = { { { 1.0f / Xs, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f / Ys, 0.0f, 0.0f },
                            { 0.0f, 0.0f, 0.0f, 1.0f / CameraNear(F) }, { 0.0f, 0.0f, 1.0f, 0.0f } } };
        return R;
    }

    // One frame's clip space into another's: back into the first camera's space, out into the level's, into the other
    // camera's, and through its projection.
    inline FMat ClipToClip(const FFrame& From, const FFrame& To)
    {
        return MatMul(MatMul(ClipToView(From), ViewToWorld(From)), MatMul(WorldToView(To), ViewToClip(To)));
    }
}

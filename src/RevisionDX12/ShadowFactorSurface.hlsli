// SecondRevision - the shadow factor's pixel, before the lights (ShadowFactor.hlsl): the statements both factors start
// with, pasted into a function body by #include - PSMain's own for the shadow maps, FactorSurface for the RT shadows.
// Not a function of its own, because it leaves early (return), and FXC builds a function's early returns into longer
// code: 534 instruction slots with the statements in PSMain itself, 583 through a function. Defines, for what follows it:
// Kind, S, N, Nw, Total, Rot and ShareNorm - the last left at 0 for the RT occlusion (RDX_RT_AO), which has no use for
// the lights' shares. The run-time compile reads it out of the DLL (FinalPass.cpp).
    uint Wd, Ht;
    WorldDepth.GetDimensions(Wd, Ht);
    float2 Size = float2(Wd, Ht);
    int2 P = min(int2(Pos.xy) * int(Scale) + int(Scale / 2u), int2(int(Wd) - 1, int(Ht) - 1));
    // Outside the world view (a camera window drawn alone, a view smaller than the screen), or within a view drawn later
    // onto another part of the screen (the spy drone's window): not the world's shadows.
    float2 Uv = (float2(P) + 0.5f) / Size;
    if (any(Uv < ViewRect.xy) || any(Uv >= ViewRect.xy + ViewRect.zw))
        return 1.0f;
    [loop] for (uint r = 0; r < SkipCount; r++)
    {
        if (all(Uv >= SkipRect[r].xy) && all(Uv < SkipRect[r].xy + SkipRect[r].zw))
            return 1.0f;
    }
    // Marks (the factor's diagnostic view only): a black dot where each light projects.
    if (Marks != 0)
    {
        [loop] for (uint m = 0; m < Count; m++)
        {
            float3 Lm = LightCam[m].xyz;
            if (Lm.z > 1.0f)
            {
                float2 LN = float2(Lm.x / (Lm.z * TanX), Lm.y / (Lm.z * TanY));
                if (distance((ViewRect.xy + (LN * 0.5f + 0.5f) * ViewRect.zw) * Size, float2(P)) < 16.0f * float(Scale))
                    return 0.0f;
            }
        }
    }
    if (NoOnTop == 0 && OnTopDepth.Load(int3(P, 0)) > 0.0f)
        return 1.0f;
    // The surface's kind (FSolidPass::SetKind): 0 the level, 1 an actor, 2 the grass's blades. What blended draws laid over the surface - a flame,
    // steam, glass - keeps its light by way of the added target in the apply pass, not here.
    uint Kind = WorldStencil.Load(int3(P, 0)).g;
    float D = WorldDepth.Load(int3(P, 0));
    if (D <= 0.0f)
        return 1.0f;
    float W = 1.0f / D;
    float2 Ndc = ViewNdc(P, Size);
    float3 S = float3(Ndc.x * TanX * W, Ndc.y * TanY * W, W);
    // The surface's normal from the depth (towards the viewer's side), as in the depth view: a surface turned away from
    // a light gets none of its light, so there is nothing to take away. At the target's edge one neighbour is the pixel
    // itself, a zero step, so the other side is taken instead: a zero step gives a NaN normal.
    int2 Mx = int2(int(Wd) - 1, int(Ht) - 1);
    float3 Rt = ViewAt(min(P + int2(1, 0), Mx), Size) - S;
    float3 Lf = S - ViewAt(max(P - int2(1, 0), int2(0, 0)), Size);
    float3 Dn = ViewAt(min(P + int2(0, 1), Mx), Size) - S;
    float3 Ab = S - ViewAt(max(P - int2(0, 1), int2(0, 0)), Size);
    float3 Dx = (P.x >= Mx.x) ? Lf : ((P.x <= 0) ? Rt : ((abs(Rt.z) < abs(Lf.z)) ? Rt : Lf));
    float3 Dy = (P.y >= Mx.y) ? Ab : ((P.y <= 0) ? Dn : ((abs(Dn.z) < abs(Ab.z)) ? Dn : Ab));
    float3 Cr = cross(Dy, Dx);
    float CrL = length(Cr);
    if (!(CrL > 1.0e-12f))
        return 1.0f;
    float3 N = Cr / CrL;
    // The grass (kind 2, FSolidPass::AddGrassDraw): its blades are thinner than a pixel off from the eye, and the depth's
    // neighbours there are now a blade, now the ground, as the picture's jitter moves - a normal from them turns from
    // frame to frame, and the blade's light with it. A blade takes the upright of the ground it stands on instead, and
    // is shaded as that ground is.
    if (Kind == 2u)
        N = float3(AxisX.z, AxisY.z, AxisZ.z);
    float3 Nw = AxisX * N.x + AxisY * N.y + AxisZ * N.z;
    // All the light this pixel receives, as the geometry pass wrote it (the light map times the vertex colour).
    float Total = SceneLight.Load(int3(P, 0));
    // A turn of the soft edge's spiral per pixel, so the samples' pattern does not show as bands.
    uint H = (uint(P.x) * 73856093u) ^ (uint(P.y) * 19349663u);
    H = (H ^ (H >> 13)) * 1274126177u;
    float Rot = float(H & 65535u) / 65536.0f * 6.2831853f;
#ifndef RDX_RT_AO
    // A pixel cannot lose more light than it receives. Every light's share is summed first, and if the sum would go
    // above one they are all scaled down in proportion. Put another way: shadows add up, and
    // lights do not cancel each other out. Without it each of ten lights claimed up to MaxShare (95%) on its own,
    // and the moment the level's geometry started blocking them too - most lights in a room are behind a wall, and a
    // light behind a wall gives this pixel nothing, so it may take nothing away - ten of those multiplied the picture
    // into black (measured: mean 44 -> 13, 87% of pixels darker).
    float SumShare = 0.0f;
    [loop] for (uint s = 0; s < Count; s++)
        SumShare += LightShare(s, S, N, Total);
    float ShareNorm = ((SumShare > 1.0f) ? (1.0f / SumShare) : 1.0f) * MaxShare;
#else
    float ShareNorm = 0.0f;
#endif

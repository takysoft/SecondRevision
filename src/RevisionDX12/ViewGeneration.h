// SecondRevision - when a pass writes its views of the backend's textures again.
#pragma once

// A pass writes the descriptors of the textures it reads into a heap of its own once, and again only when a texture
// changes - which it tells by the texture's pointer. That alone is no proof: a texture made right after another was
// released may be given the released one's address, and a view of the released one is a GPU fault waiting to happen.
// So every place that makes such a texture again counts it here (RdxViewsChanged), and a pass whose views were written
// at an older count writes them all anew.
inline INT& RdxViewGenerationRef()
{
    static INT Generation = 1;
    return Generation;
}

inline INT  RdxViewGeneration() { return RdxViewGenerationRef(); }
inline void RdxViewsChanged()   { RdxViewGenerationRef()++; }

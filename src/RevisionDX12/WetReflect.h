// SecondRevision - the wet floors' late reflection pass (WetReflect.hlsl; docs/terv-nedves-feluletek.md 8.6).
//
// The floors are drawn early, in the engine's order, when much of what they should mirror is not on the picture yet. So
// the wet floors' runs only darken the stone and mark their pixels (SolidPass.hlsl's WetMark), and this pass gives back
// the world over those pixels once the opaque world and the actors are drawn and copied (FBackend::CopyForWater) - before
// anything see-through, which is drawn over it as over any floor. One full-screen triangle; the pixels with no mark of
// theirs are left alone.
#pragma once

class FWetReflect
{
public:
    FWetReflect();
    ~FWetReflect();

    // The root signature and the pipelines, for the scene's targets as they are bound: Picture's format, with the
    // shadows' light and added targets (LightFormat) or without, one sample and Samples. False, said in the log, where
    // they could not be made.
    UBOOL Init(ID3D12Device* Device, DXGI_FORMAT Picture, DXGI_FORMAT LightFormat, DXGI_FORMAT DepthFormat, INT Samples);
    void  Shutdown();
    UBOOL IsReady() const { return Root.Get() != nullptr; }

    // The pass's numbers: the wet block (RdxMath::WetBlockFloats, as the frame's wet runs read it), then the copies'
    // size and one over it; the film's share, the damp stone's spread, the streaks' strength and their roughness on the
    // damp stone; their roughness on the water, and three spare; whether the water's image is the rays' this frame (1),
    // and three spare.
    enum { CONSTANTS = RdxMath::WetBlockFloats + 16 };
    // Drawn over the scene's targets bound now - three with the shadows' (bThree), else the picture alone; bMsaa: they
    // are the multisampled world's. The copies of the picture and the depth, the mark, and the rain's cover
    // (FBackend::SetWetCover), as views in Heap; the lamps whose streaks the floors give back (FBackend::SetWetLamps) at
    // Lamps, a constant buffer; the own lamps, the flashlight's and the weapon lamps', at Spots (WetReflect.hlsl's
    // WetSpots).
    void Draw(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, const FLOAT* Constants,
              D3D12_GPU_DESCRIPTOR_HANDLE Picture, D3D12_GPU_DESCRIPTOR_HANDLE Depth, D3D12_GPU_DESCRIPTOR_HANDLE Mark,
              D3D12_GPU_DESCRIPTOR_HANDLE Cover, D3D12_GPU_VIRTUAL_ADDRESS Lamps, D3D12_GPU_VIRTUAL_ADDRESS Spots,
              UBOOL bThree, UBOOL bMsaa, UBOOL bFlames = 0);

private:
    TComRef<ID3D12RootSignature> Root;
    // [the shader: the image (PSWet), the fires' flames (PSWetFlames, drawn with the walk's picture in Picture's
    // place)][the targets: one, three][one sample, multisampled]
    TComRef<ID3D12PipelineState> Pipelines[2][2][2];
};

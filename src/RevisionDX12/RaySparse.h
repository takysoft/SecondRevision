// SecondRevision - Reflections=RTLow: of the rays a round's pixels wrote down, the ones the 64-bit host traces - about
// one of each three by three pixels (RaySparse.hlsl; docs/terv-nedves-feluletek.md 7.2).
//
// The pixels' hardware fours give RTHalf its two by two while they draw (SolidPass.hlsl, RtAskLed); no group of the
// card's holds three by three pixels for certain. So every pixel writes its rays down - into a list of this
// process's own (FHostRayFeed::GetLow) - and this pass goes over all of it after them and before the host takes its
// list (FBackend::EndMirrorAsk): of each three by three, the blocks alike to the lender pixel's borrow its - the
// middle's, or the first asking pixel's where the middle asks nothing -, and the rest are kept; the kept ones'
// requests go one after another into the host's list, which the host traces as it traces any. The build that reads
// the answers finds a block in the own list and its answers where the pass put its requests (RtFind).
#pragma once

class FRaySparse
{
public:
    FRaySparse();
    ~FRaySparse();

    // The root signature and the pipeline. False, said in the log, where they could not be made.
    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Pipeline.Get() != nullptr; }
    // The pass on the round's lists: HeadsView - the pixels' heads as a UAV in Heap, a shader-visible heap; Low the own
    // list, all the pixels asked; Traced the host's, emptied, which the kept requests go into. Width x Height the heads'
    // size; Eye the camera in the level's space. The three are in UNORDERED_ACCESS before and after; what the pixels
    // wrote is done before the pass reads it.
    void  Run(ID3D12GraphicsCommandList* Cmd, ID3D12DescriptorHeap* Heap, D3D12_GPU_DESCRIPTOR_HANDLE HeadsView, INT Width,
              INT Height, const FLOAT* Eye, ID3D12Resource* Heads, ID3D12Resource* Low, ID3D12Resource* Traced);

private:
    enum { CONSTANTS = 8 };
    TComRef<ID3D12RootSignature> Root;
    TComRef<ID3D12PipelineState> Pipeline;
};

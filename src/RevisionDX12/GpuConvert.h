// SecondRevision - palettised textures converted on the GPU: the expansion and the masked-edge
// dilation as compute shaders, giving byte for byte what the CPU path gives. The reasoning is in
// docs/terv-gpu-atalakitas.md. The CPU path stays: as the fallback, and as the reference the
// results are checked against.
#pragma once

class FGpuConvert
{
public:
    FGpuConvert() {}
    ~FGpuConvert() { Shutdown(); }

    UBOOL Init(ID3D12Device* Device);
    void  Shutdown();
    UBOOL IsReady() const { return Close.Get() != nullptr; }

    // One level of a texture: its size, where its indices start in the source (W bytes to a
    // row, packed), and where it goes in Out and Temp, rows Pitch bytes apart - the upload
    // layout.
    struct FLevel
    {
        UINT W, H, IndexOff, Off, Pitch;
    };

    // Every level of one texture. The indices at SrcVA + each level's IndexOff, the 256
    // ready-made texels at SrcVA + TableOff; the result into Out. Masked: then dilated in Passes
    // passes, ping-ponging between Out and Temp (same layout, same size), and closed - the
    // result always ends in Out. Each step runs on every level before the next step begins: the
    // levels lie apart, so one UAV barrier a step serves them all, where one a step and a level
    // would be five a level - sixty on a 12-level chain. Out and Temp are used as UAVs: in
    // COMMON they are promoted, otherwise they must be in UNORDERED_ACCESS. The caller puts a
    // barrier before anything reads Out.
    void RecordLevels(ID3D12GraphicsCommandList* Cmd, D3D12_GPU_VIRTUAL_ADDRESS SrcVA, UINT TableOff,
                      const FLevel* Levels, INT LevelCount,
                      ID3D12Resource* Out, ID3D12Resource* Temp, UBOOL bMasked, INT Passes);

private:
    TComRef<ID3D12RootSignature> Root;
    TComRef<ID3D12PipelineState> Expand;
    TComRef<ID3D12PipelineState> Dilate;
    TComRef<ID3D12PipelineState> Close;
};

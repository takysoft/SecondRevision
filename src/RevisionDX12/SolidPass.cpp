#include "Pch.h"
#include "Backend.h"
#include "SolidPass.h"
#include "TextureCache.h"
#include "Log.h"
#include "RenderMath.h"

#include <d3dcompiler.h>
#include "ShaderCache.h"
#include <stdio.h>                  // swprintf_s: the run markers for DRED

// The shaders' text (SolidPass.hlsl), put into the DLL by the build (scripts/embed-text.ps1): compiled at load time like
// every other pass. The stages of the runs on rays the build compiles itself, with dxc (SolidPassRt*.h): the ones
// before the pixel shader, and each run's two pixel shaders for the 64-bit host's rays - the one that writes its rays
// down (Ask) and the one that reads the answers (Host).
#include "SolidPassText.h"
#include "GrassShader.h"
#include "TreeShader.h"
#include "DustShader.h"
#include "SolidPassRtVS.h"
#include "SolidPassRtHS.h"
#include "SolidPassRtDS.h"
#include "SolidPassRtMarkPS.h"
#include "SolidPassRtMarkPSLit.h"
#include "SolidPassRtFlashVS.h"
#include "SolidPassRtMainVS.h"
#include "SolidPassRtAskGlassMirrorPS.h"
#include "SolidPassRtHostGlassMirrorPS.h"
#include "SolidPassRtHostGlassMirrorPSLit.h"
#include "SolidPassRtHostPS.h"
#include "SolidPassRtHostPSLit.h"
#include "SolidPassRtAskPS.h"
#include "SolidPassRtAskMarkPS.h"
#include "SolidPassRtHostSludgeMirrorPS.h"
#include "SolidPassRtHostSludgeMirrorPSLit.h"
#include "SolidPassRtAskSludgeMirrorPS.h"
#include "SolidPassRtHostSplashPS.h"
#include "SolidPassRtHostSplashPSLit.h"
#include "SolidPassRtAskSplashPS.h"
#include "SolidPassRtHostPaneKeepPS.h"
#include "SolidPassRtAskPaneKeepPS.h"

FSolidPass::FSolidPass()
{
    Colours = RdxColoursFor(RdxMath::Hdr_Off);
    appMemzero(VertexBuffers, sizeof(VertexBuffers));
    appMemzero(Mapped, sizeof(Mapped));
    appMemzero(Runs, sizeof(Runs));
    Dest              = nullptr;
    VertexCount       = 0;
    RunCount          = 0;
    Width             = 1;
    Height            = 1;
    MsaaSamples       = 1;
    bMsaaLive         = 0;
    bOverflowReported = 0;
    appMemzero(WaterNow, sizeof(WaterNow));
    WaterTable        = 0;
    WaterLamps        = 0;
    SplashSmoothView  = -1;
    SplashThickView   = -1;
}

FSolidPass::~FSolidPass()
{
    Shutdown();
}

// ---------------------------------------------------------------------------

UBOOL FSolidPass::CreateRootSignature(ID3D12Device* Device)
{
    if (!MakeRoot(Device, 0, RootSignature))
        return 0;
    // The rays' own, where the 64-bit host traces them (SetRtHosted): the runs that write their rays down and those
    // that read its answers bind by it. Without it Reflections=RT runs as High, and nothing else changes.
    RtWaterRoot.Release();
    if (bRtHosted && !MakeRoot(Device, 1, RtWaterRoot))
        RdxLog(TEXT("water: the RT water's root signature was refused - Reflections=RT runs as High"));
    return 1;
}

UBOOL FSolidPass::MakeRoot(ID3D12Device* Device, UBOOL bRt, TComRef<ID3D12RootSignature>& Out)
{
    D3D12_DESCRIPTOR_RANGE Range;
    appMemzero(&Range, sizeof(Range));
    Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors     = 1;
    Range.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE LightRange;
    appMemzero(&LightRange, sizeof(LightRange));
    LightRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    LightRange.NumDescriptors     = 1;
    LightRange.BaseShaderRegister = 1;

    // The water's views (BLEND_Water): the picture, the depth, the ripples and the texture's own wave field, side by side
    // in the texture heap's reserved views.
    // The caustics' net after them (t13): a second range of the same table, which costs a root signature nothing - the
    // RT water's has no room for another parameter.
    D3D12_DESCRIPTOR_RANGE WaterRange[2];
    appMemzero(WaterRange, sizeof(WaterRange));
    WaterRange[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    WaterRange[0].NumDescriptors     = 4;
    WaterRange[0].BaseShaderRegister = 2;
    WaterRange[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    WaterRange[1].NumDescriptors     = 1;
    WaterRange[1].BaseShaderRegister = 13;
    WaterRange[1].OffsetInDescriptorsFromTableStart = 4;

    // The water's own depth, which the see-through water writes (u3 - shader model 5.0 numbers a pixel shader's UAVs
    // after its outputs, FTargetSwitch::WaterSurfaceView): a table, as its view sits in the texture heap. Last in both
    // root signatures - in the RT water's it is the 64th DWORD, the most a root signature may have.
    // In the rays' signature, a second range of the same table for the builds that write their rays down for the
    // 64-bit host (SetRayListViews): the pixels' heads and the requests (u4, u5), as far after the depth's view in
    // the texture heap as the backend put them. The signature has no room for a parameter more.
    // In both, a range for what the shadows' apply pass needs of the water's pixels (u6, WaterShade), as far after the
    // depth's view as SetWaterShadeView says.
    D3D12_DESCRIPTOR_RANGE SurfaceRange[3];
    appMemzero(SurfaceRange, sizeof(SurfaceRange));
    SurfaceRange[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    SurfaceRange[0].NumDescriptors     = 1;
    SurfaceRange[0].BaseShaderRegister = 3;
    SurfaceRange[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    SurfaceRange[1].NumDescriptors     = 1;
    SurfaceRange[1].BaseShaderRegister = 6;
    SurfaceRange[1].OffsetInDescriptorsFromTableStart = (UINT)WaterShadeAfterSurface;
    SurfaceRange[2].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    SurfaceRange[2].NumDescriptors     = 2;
    SurfaceRange[2].BaseShaderRegister = 4;
    SurfaceRange[2].OffsetInDescriptorsFromTableStart = (UINT)RayHeadsAfterSurface;
    const UBOOL bRayList = (bRt && RayHeadsAfterSurface > 0 && RayAnswersAfterMasks > 1) ? 1 : 0;

    D3D12_ROOT_PARAMETER Params[14];
    appMemzero(Params, sizeof(Params));

    Params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[0].Constants.ShaderRegister = 0;
    Params[0].Constants.Num32BitValues = 6;      // 2/width, 2/height, the jitter's two (SetJitter), the lamps' boost, the mip bias
    // Both stages: the first four are the vertex shader's, the lamps' boost is the pixel shader's (SetLightBoost).
    Params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[1].DescriptorTable.NumDescriptorRanges = 1;
    Params[1].DescriptorTable.pDescriptorRanges   = &Range;
    Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[2].DescriptorTable.NumDescriptorRanges = 1;
    Params[2].DescriptorTable.pDescriptorRanges   = &LightRange;
    Params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Per run: whether the texture is sampled unfiltered (PF_NoSmooth), for a screen-tint run its fog colour (EndFlash),
    // and for a water run its own constants (SetWaterConstants).
    Params[3].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[3].Constants.ShaderRegister = 1;
    Params[3].Constants.Num32BitValues = 4 + WATER_CONSTANTS;
    Params[3].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;      // the water's domain shader reads them too

    Params[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[4].DescriptorTable.NumDescriptorRanges = 2;
    Params[4].DescriptorTable.pDescriptorRanges   = WaterRange;
    Params[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;      // the domain shader lifts by the fields

    // The water's lamps (FBackend::SetWaterLamps): a constant buffer of this frame slot's, for the water's glints. A relief
    // run binds its own there - the relief's lamps, and for its patches the held edges DSRelief measures its rim band
    // from (SetReliefHeld) - so the domain shader reads it too.
    Params[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    Params[5].Descriptor.ShaderRegister = 2;
    Params[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // The RT water's (FRtWaterBind): the scene (t6), the rays' table (t14, a buffer as long as the frame's geometries
    // ask), the casters' texture places - the frame's (t7) and the arena's (t8) - the masks (t9) and the far probe
    // (t11), views side by side in the texture heap and so one table of two; the level's colours (t10); and the
    // texture heap itself from its first view on, as an array without an end (t0 of space 1): a mirror ray reads the
    // texture of what it met by its place there. With the pass's own and the reactive mask's table, all 64 DWORDs a
    // root signature may have - the masks and the probe share a table to make the array's place.
    // And for the builds that read the host's answers (SetRayListViews), a third range of that table: the answers
    // and what the rays passed (t16, t17), as far after the masks' view as the backend put them - the pixels' heads
    // are in the masks' own place there.
    // And the own lamps' table and maps (t18, t19, SetOwnLampViews), as far after the masks' view as the backend put
    // them: a range of that table, which costs the full signature nothing.
    D3D12_DESCRIPTOR_RANGE MaskRange[4];
    appMemzero(MaskRange, sizeof(MaskRange));
    MaskRange[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    MaskRange[0].NumDescriptors     = 1;
    MaskRange[0].BaseShaderRegister = 9;
    MaskRange[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    MaskRange[1].NumDescriptors     = 1;
    MaskRange[1].BaseShaderRegister = 11;
    MaskRange[1].OffsetInDescriptorsFromTableStart = 1;
    MaskRange[2].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    MaskRange[2].NumDescriptors     = 2;
    MaskRange[2].BaseShaderRegister = 18;
    MaskRange[2].OffsetInDescriptorsFromTableStart = (UINT)OwnAfterMasks;
    MaskRange[3].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    MaskRange[3].NumDescriptors     = 2;
    MaskRange[3].BaseShaderRegister = 16;
    MaskRange[3].OffsetInDescriptorsFromTableStart = (UINT)RayAnswersAfterMasks;
    D3D12_DESCRIPTOR_RANGE HeapRange;
    appMemzero(&HeapRange, sizeof(HeapRange));
    HeapRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    HeapRange.NumDescriptors     = (UINT)-1;
    HeapRange.BaseShaderRegister = 0;
    HeapRange.RegisterSpace      = 1;
    for (INT k = 6; k < 13; k++)
        Params[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    Params[6].ParameterType              = D3D12_ROOT_PARAMETER_TYPE_SRV;
    Params[6].Descriptor.ShaderRegister  = 6;
    Params[7].ParameterType              = D3D12_ROOT_PARAMETER_TYPE_SRV;
    Params[7].Descriptor.ShaderRegister  = 14;
    Params[8].ParameterType              = D3D12_ROOT_PARAMETER_TYPE_SRV;
    Params[8].Descriptor.ShaderRegister  = 7;
    Params[9].ParameterType              = D3D12_ROOT_PARAMETER_TYPE_SRV;
    Params[9].Descriptor.ShaderRegister  = 8;
    Params[10].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[10].DescriptorTable.NumDescriptorRanges = bRayList ? 4 : 3;
    Params[10].DescriptorTable.pDescriptorRanges   = MaskRange;
    Params[11].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    Params[11].Descriptor.ShaderRegister = 10;
    Params[12].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[12].DescriptorTable.NumDescriptorRanges = 1;
    Params[12].DescriptorTable.pDescriptorRanges   = &HeapRange;

    // The relief's covers (t12, FBackend's ReliefCover), for its domain shader (DSRelief): the pass's own root signature
    // only, in the place the RT water's own parameters start - no RT water run draws relief. A table, as the covers' view
    // sits in the texture heap.
    D3D12_DESCRIPTOR_RANGE CoverRange;
    appMemzero(&CoverRange, sizeof(CoverRange));
    CoverRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    CoverRange.NumDescriptors     = 1;
    CoverRange.BaseShaderRegister = 12;
    if (!bRt)
    {
        appMemzero(&Params[6], sizeof(Params[6]));
        Params[6].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[6].DescriptorTable.NumDescriptorRanges = 1;
        Params[6].DescriptorTable.pDescriptorRanges   = &CoverRange;
        Params[6].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_DOMAIN;
    }
    D3D12_ROOT_PARAMETER& Surface = Params[bRt ? ROOT_WATER_UAV_RT : ROOT_WATER_UAV];
    appMemzero(&Surface, sizeof(Surface));
    Surface.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Surface.DescriptorTable.NumDescriptorRanges = bRayList ? 3 : 2;
    Surface.DescriptorTable.pDescriptorRanges   = SurfaceRange;
    Surface.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    // The flashlight's block (b4, SetBeam), in the pass's own root signature after everything else it has; and the
    // mirrors' map after it (t15, SetBounceView), with the own lamps' table and maps a range of the same table (t18,
    // t19, as far after the map's view as SetOwnLampViews says).
    D3D12_DESCRIPTOR_RANGE BounceRange[2];
    appMemzero(BounceRange, sizeof(BounceRange));
    BounceRange[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    BounceRange[0].NumDescriptors     = 1;
    BounceRange[0].BaseShaderRegister = 15;
    BounceRange[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    BounceRange[1].NumDescriptors     = 2;
    BounceRange[1].BaseShaderRegister = 18;
    BounceRange[1].OffsetInDescriptorsFromTableStart = (UINT)OwnAfterBounce;
    if (!bRt)
    {
        appMemzero(&Params[ROOT_BEAM], sizeof(Params[ROOT_BEAM]));
        Params[ROOT_BEAM].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[ROOT_BEAM].Descriptor.ShaderRegister = 4;
        Params[ROOT_BEAM].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
        appMemzero(&Params[ROOT_BOUNCE], sizeof(Params[ROOT_BOUNCE]));
        Params[ROOT_BOUNCE].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[ROOT_BOUNCE].DescriptorTable.NumDescriptorRanges = 2;
        Params[ROOT_BOUNCE].DescriptorTable.pDescriptorRanges   = BounceRange;
        Params[ROOT_BOUNCE].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // Anisotropic, 16x - the stock renderer's own setting (Anisotropy=16) - with wrap,
    // now that every texture has its mip chain. Wrap is not a preference: measurement showed surfaces tiling
    // their texture up to 512 times (docs/spec-04 section 2.3), so clamping would
    // visibly smear every large wall and floor. Bilinear is why masked textures need
    // their transparent texels recoloured - see RdxMath::DilateTransparentEdges.
    D3D12_STATIC_SAMPLER_DESC Sampler;
    appMemzero(&Sampler, sizeof(Sampler));
    Sampler.Filter         = D3D12_FILTER_ANISOTROPIC;
    Sampler.MaxAnisotropy  = 16;
    Sampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Sampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Sampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Sampler.MaxLOD         = D3D12_FLOAT32_MAX;
    Sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    Sampler.ShaderRegister = 0;
    Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;     // the water's domain shader samples its fields with it

    // The same, unfiltered, for PF_NoSmooth ("don't smooth textures", UnObj.h). The HUD
    // uses it: the stock renderer shows the icons' texels as crisp squares.
    D3D12_STATIC_SAMPLER_DESC Samplers[2];
    Samplers[0] = Sampler;
    Samplers[1] = Sampler;
    Samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
    Samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.NumParameters     = bRt ? (ROOT_WATER_UAV_RT + 1) : (ROOT_BOUNCE + 1);
    Desc.pParameters       = Params;
    Desc.NumStaticSamplers = 2;
    Desc.pStaticSamplers   = Samplers;
    Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    TComRef<ID3DBlob> Blob, Error;
    if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("root signature serialise failed"));
        return 0;
    }

    if (FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), Out.VoidSlot())))
    {
        RdxLog(TEXT("CreateRootSignature failed"));
        return 0;
    }
    return 1;
}

// Grass3D: the opaque pipeline once more with the blades' vertex shader in front of the same pixel shader - so a blade
// writes the depth, the stencil kind, the light target and the added one exactly as the ground under it does - a set
// each. The blades are made from nothing but their instance and vertex numbers: no vertex input at all.
void FSolidPass::MakeGrass(ID3D12Device* Device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Opaque)
{
    GrassPipelines[0].Release();
    GrassPipelines[1].Release();
    if (!GrassVs)
        return;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = Opaque;
    Desc.VS.pShaderBytecode          = GrassVs->GetBufferPointer();
    Desc.VS.BytecodeLength           = GrassVs->GetBufferSize();
    Desc.InputLayout.pInputElementDescs = nullptr;
    Desc.InputLayout.NumElements        = 0;
    Desc.SampleDesc.Count            = 1;
    HRESULT Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                       GrassPipelines[0].VoidSlot());
    if (SUCCEEDED(Made) && MsaaSamples > 1)
    {
        Desc.SampleDesc.Count = (UINT)MsaaSamples;
        Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                   GrassPipelines[1].VoidSlot());
    }
    if (FAILED(Made))
    {
        RdxLog(TEXT("the grass's pipeline could not be made (0x%08X) - no grass"), (UINT)Made);
        GrassPipelines[0].Release();
        GrassPipelines[1].Release();
    }
}

// Trees3D: the opaque pipeline once more with the plants' vertex shader and their vertices (TreeGen.h's FVertex: place,
// normal, picture, and the wind's three - how far it moves, its branch's beat, the crown's shade) - so bark and leaves
// write the depth, the stencil kind and the light target as an actor's mesh does. A set each.
void FSolidPass::MakeTree(ID3D12Device* Device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Opaque)
{
    TreePipelines[0].Release();
    TreePipelines[1].Release();
    if (!TreeVs)
        return;
    // TreeGen.h's FVertex: its place, normal, place on the bark picture, the wind's reach and beat, and how open the
    // plant is round it.
    D3D12_INPUT_ELEMENT_DESC Elements[6];
    appMemzero(Elements, sizeof(Elements));
    Elements[0].SemanticName      = "POSITION";
    Elements[0].Format            = DXGI_FORMAT_R32G32B32_FLOAT;
    Elements[0].AlignedByteOffset = 0;
    Elements[1].SemanticName      = "NORMAL";
    Elements[1].Format            = DXGI_FORMAT_R32G32B32_FLOAT;
    Elements[1].AlignedByteOffset = 12;
    Elements[2].SemanticName      = "TEXCOORD";
    Elements[2].SemanticIndex     = 0;
    Elements[2].Format            = DXGI_FORMAT_R32G32_FLOAT;
    Elements[2].AlignedByteOffset = 24;
    // Its sway in the wind: its chain's sums as halfs (TreeGen.h's FSway), four and three.
    Elements[3].SemanticName      = "TEXCOORD";
    Elements[3].SemanticIndex     = 1;
    Elements[3].Format            = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Elements[3].AlignedByteOffset = 32;
    Elements[4].SemanticName      = "TEXCOORD";
    Elements[4].SemanticIndex     = 2;
    Elements[4].Format            = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Elements[4].AlignedByteOffset = 40;
    Elements[5].SemanticName      = "COLOR";
    Elements[5].Format            = DXGI_FORMAT_R8G8B8A8_UNORM;
    Elements[5].AlignedByteOffset = 48;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = Opaque;
    Desc.VS.pShaderBytecode             = TreeVs->GetBufferPointer();
    Desc.VS.BytecodeLength              = TreeVs->GetBufferSize();
    Desc.InputLayout.pInputElementDescs = Elements;
    Desc.InputLayout.NumElements        = 6;
    Desc.SampleDesc.Count               = 1;
    HRESULT Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                       TreePipelines[0].VoidSlot());
    if (SUCCEEDED(Made) && MsaaSamples > 1)
    {
        Desc.SampleDesc.Count = (UINT)MsaaSamples;
        Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                   TreePipelines[1].VoidSlot());
    }
    if (FAILED(Made))
    {
        RdxLog(TEXT("the trees' pipeline could not be made (0x%08X) - the game's own plants"), (UINT)Made);
        TreePipelines[0].Release();
        TreePipelines[1].Release();
    }
    // The leaves': each instance a leaf (TreeGen.h's FLeafInstance, 56 bytes), the corner its vertex's number.
    LeafPipelines[0].Release();
    LeafPipelines[1].Release();
    if (!LeafVs || FAILED(Made))
        return;
    D3D12_INPUT_ELEMENT_DESC Leaf[8];
    appMemzero(Leaf, sizeof(Leaf));
    const char* const Names[8]   = { "POSITION", "TEXCOORD", "NORMAL", "TANGENT", "COLOR", "TEXCOORD", "TEXCOORD",
                                     "TEXCOORD" };
    const UINT        Indices[8] = { 0, 0, 0, 0, 0, 1, 2, 3 };
    const DXGI_FORMAT Formats[8] = { DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                     DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
    const UINT Offsets[8]        = { 0, 12, 16, 24, 32, 36, 40, 48 };
    for (INT k = 0; k < 8; k++)
    {
        Leaf[k].SemanticName         = Names[k];
        Leaf[k].SemanticIndex        = Indices[k];
        Leaf[k].Format               = Formats[k];
        Leaf[k].AlignedByteOffset    = Offsets[k];
        Leaf[k].InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        Leaf[k].InstanceDataStepRate = 1;
    }
    Desc.VS.pShaderBytecode             = LeafVs->GetBufferPointer();
    Desc.VS.BytecodeLength              = LeafVs->GetBufferSize();
    Desc.InputLayout.pInputElementDescs = Leaf;
    Desc.InputLayout.NumElements        = 8;
    Desc.SampleDesc.Count               = 1;
    Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState), LeafPipelines[0].VoidSlot());
    if (SUCCEEDED(Made) && MsaaSamples > 1)
    {
        Desc.SampleDesc.Count = (UINT)MsaaSamples;
        Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState), LeafPipelines[1].VoidSlot());
    }
    if (FAILED(Made))
    {
        RdxLog(TEXT("the leaves' pipeline could not be made (0x%08X) - the game's own plants"), (UINT)Made);
        LeafPipelines[0].Release();
        LeafPipelines[1].Release();
    }
}

// Wind: the see-through pipeline once more with the dust's vertex shader - the motes laid over the world as its other
// see-through things are, by the same pixel shader. A set each. No vertex input: the motes are made from their numbers.
void FSolidPass::MakeDust(ID3D12Device* Device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Translucent)
{
    DustPipelines[0].Release();
    DustPipelines[1].Release();
    if (!DustVs)
        return;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = Translucent;
    Desc.VS.pShaderBytecode             = DustVs->GetBufferPointer();
    Desc.VS.BytecodeLength              = DustVs->GetBufferSize();
    Desc.InputLayout.pInputElementDescs = nullptr;
    Desc.InputLayout.NumElements        = 0;
    Desc.SampleDesc.Count               = 1;
    HRESULT Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                       DustPipelines[0].VoidSlot());
    if (SUCCEEDED(Made) && MsaaSamples > 1)
    {
        Desc.SampleDesc.Count = (UINT)MsaaSamples;
        Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                   DustPipelines[1].VoidSlot());
    }
    if (FAILED(Made))
    {
        RdxLog(TEXT("the dust's pipeline could not be made (0x%08X) - no dust"), (UINT)Made);
        DustPipelines[0].Release();
        DustPipelines[1].Release();
    }
}

UBOOL FSolidPass::CreatePipelineState(ID3D12Device* Device)
{
    TComRef<ID3DBlob> Vs, Ps, Error;
    const UINT Flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const SIZE_T Len = sizeof(GSolidPassText) - 1;

    // Shader-side switches come from build defines, the same way the C++ ones do, so a
    // diagnostic build is one flag away and can never ship by accident.
    //
    // RDX_LIGHT_SCALE: how a light map value maps to a brightness multiplier. This is
    // NOT a guess to be left alone - it is calibrated against the stock renderer by
    // comparing the same frame, and the current value is recorded in docs/meres-03.
    // The shader needs the same number as the neutral light map texel does, so it comes from
    // the one definition in RenderMath.h rather than a second copy that could drift.
#define RDX_STRINGIFY2(x) #x
#define RDX_STRINGIFY(x)  RDX_STRINGIFY2(x)
#ifndef RDX_DEBUG_LIGHTMAP
#define RDX_DEBUG_LIGHTMAP 0
#endif
#ifndef RDX_DEBUG_DIFFUSE
#define RDX_DEBUG_DIFFUSE 0
#endif
#ifndef RDX_DEBUG_NODEPTH
#define RDX_DEBUG_NODEPTH 0
#endif
#ifndef RDX_DEBUG_NOCLIP
#define RDX_DEBUG_NOCLIP 0
#endif
#ifndef RDX_MODULATED
#define RDX_MODULATED 0
#endif

    // With stencil kinds (the shadows) RDX_LIGHT_TARGET adds the second target's entry point, PSMainLit. Without them the
    // macro is not even defined, and the pixel shaders come from PSMain: the vanilla path compiles what it always did.
    D3D_SHADER_MACRO Macros[10];
    Macros[0].Name       = "RDX_LIGHT_SCALE";
    Macros[0].Definition = RDX_STRINGIFY(RDX_LIGHT_SCALE_VALUE);
    Macros[1].Name       = "RDX_DEBUG_LIGHTMAP";
    Macros[1].Definition = RDX_DEBUG_LIGHTMAP ? "1" : "0";
    Macros[2].Name       = "RDX_DEBUG_DIFFUSE";
    Macros[2].Definition = RDX_DEBUG_DIFFUSE ? "1" : "0";
    Macros[3].Name       = "RDX_DEBUG_NOCLIP";
    Macros[3].Definition = RDX_DEBUG_NOCLIP ? "1" : "0";
    // The ripples' field and the water's cutting (RdxMath), for the water's shaders. Before the light target's macro: the
    // modulated shader's list below copies the first eight and ends with that one.
    Macros[4].Name       = "RDX_RIPPLE_WORLD";
    Macros[4].Definition = RDX_STRINGIFY(RDX_RIPPLE_WORLD_VALUE);
    Macros[5].Name       = "RDX_RIPPLE_TEXELS";
    Macros[5].Definition = RDX_STRINGIFY(RDX_RIPPLE_TEXELS_VALUE);
    Macros[6].Name       = "RDX_WATER_EDGE";
    Macros[6].Definition = RDX_STRINGIFY(RDX_WATER_EDGE_VALUE);
    // The relief's lamp rows (RenderLimits.h).
    Macros[7].Name       = "RDX_RELIEF_LAMP_MAX";
    Macros[7].Definition = RDX_STRINGIFY(RDX_RELIEF_LAMP_MAX);
    // Last before the end: it is there only with the stencil kinds, and without them it ends the list itself.
    Macros[8].Name       = bStencilKinds ? "RDX_LIGHT_TARGET" : nullptr;
    Macros[8].Definition = bStencilKinds ? "1" : nullptr;
    Macros[9].Name       = nullptr;
    Macros[9].Definition = nullptr;
    const char* const PsEntry = bStencilKinds ? "PSMainLit" : "PSMain";

    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "VSMain", "vs_5_0", Flags, 0, Vs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("vertex shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, PsEntry, "ps_5_0", Flags, 0, Ps.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("pixel shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    // With the shadows' targets a draw that is the surface has an entry point of its own (PSMainLitSurface): what it
    // puts into the added target is not its picture.
    TComRef<ID3DBlob> PsSurface;
    if (bStencilKinds && FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "PSMainLitSurface",
                                           "ps_5_0", Flags, 0, PsSurface.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("the surfaces' pixel shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    // Grass3D: the blades' vertex shader (GrassShader.h), taken by the opaque pipeline's copy below (MakeGrass). Without
    // it there is no grass, and nothing else changes.
    GrassVs.Release();
    if (FAILED(RdxCompile(GGrassShader, strlen(GGrassShader), "Grass", nullptr, nullptr, "VSGrass", "vs_5_0", Flags, 0,
                          GrassVs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("the grass's vertex shader failed - no grass: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        GrassVs.Release();
    }
    // Trees3D: the plants' vertex shader (TreeShader.h), the same way (MakeTree). Without it the game's own plants stay.
    TreeVs.Release();
    if (FAILED(RdxCompile(GTreeShader, strlen(GTreeShader), "Tree", nullptr, nullptr, "VSTree", "vs_5_0", Flags, 0,
                          TreeVs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("the trees' vertex shader failed - the game's own plants: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        TreeVs.Release();
    }
    LeafVs.Release();
    if (FAILED(RdxCompile(GTreeShader, strlen(GTreeShader), "Leaf", nullptr, nullptr, "VSLeaf", "vs_5_0", Flags, 0,
                          LeafVs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("the leaves' vertex shader failed - the game's own plants: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        LeafVs.Release();
    }
    // The plants' motion vectors' shaders (PlantMotion.h): without them the plants have the camera's vector alone.
    {
        const char* const Sources[5]  = { GGrassShader, GTreeShader, GTreeShader, GTreeShader, GTreeShader };
        const char* const Entries[5]  = { "VSGrassMotion", "VSTreeMotion", "VSLeafMotion", "PSPlantMotion",
                                          "PSLeafMotion" };
        const char* const Profiles[5] = { "vs_5_0", "vs_5_0", "vs_5_0", "ps_5_0", "ps_5_0" };
        for (INT k = 0; k < 5; k++)
        {
            TComRef<ID3DBlob>& Blob = (k < 3) ? PlantMotionVs[k] : PlantMotionPs[k - 3];
            Blob.Release();
            if (FAILED(RdxCompile(Sources[k], strlen(Sources[k]), "PlantMotion", nullptr, nullptr, Entries[k],
                                  Profiles[k], Flags, 0, Blob.Slot(), Error.Slot())))
            {
                RdxLog(TEXT("the plants' motion shader %s failed - their camera's vector alone: %s"),
                       appFromAnsi(Entries[k]),
                       Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
                Blob.Release();
            }
        }
        PlantMotionState = 0;
    }
    // Wind: the dust's vertex shader (DustShader.h), the same way (MakeDust). Without it there is no dust.
    DustVs.Release();
    if (FAILED(RdxCompile(GDustShader, strlen(GDustShader), "Dust", nullptr, nullptr, "VSDust", "vs_5_0", Flags, 0,
                          DustVs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("the dust's vertex shader failed - no dust: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        DustVs.Release();
    }

    // The modulated path needs its own pixel shader: the factor of two belongs to the
    // blend mode, not to the geometry, so it cannot be decided per vertex.
    TComRef<ID3DBlob> PsMod;
    D3D_SHADER_MACRO ModMacros[11];
    for (INT m = 0; m < 8; m++) ModMacros[m] = Macros[m];
    ModMacros[8].Name        = "RDX_MODULATED";
    ModMacros[8].Definition  = "1";
    ModMacros[9]             = Macros[8];     // the light target's macro, or the end of the list
    ModMacros[10].Name       = nullptr;
    ModMacros[10].Definition = nullptr;
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", ModMacros, nullptr, PsEntry, "ps_5_0", Flags, 0, PsMod.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("modulated pixel shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    // The fog map's pixel shader (BLEND_Fog): the map's colour and alpha as they are - no light map, no clip.
    TComRef<ID3DBlob> PsFog;
    D3D_SHADER_MACRO FogMacros[11];
    for (INT m = 0; m < 11; m++) FogMacros[m] = ModMacros[m];
    FogMacros[8].Name = "RDX_FOG";
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", FogMacros, nullptr, PsEntry, "ps_5_0", Flags, 0, PsFog.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("fog map pixel shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    // The water's own pixel shader (BLEND_Water): the same geometry, over the copies of the picture and the depth.
    TComRef<ID3DBlob> PsWater;
    const char* const WaterEntry = bStencilKinds ? "PSWaterLit" : "PSWater";
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, WaterEntry, "ps_5_0", Flags, 0, PsWater.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("water pixel shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    // And the shader of its first go over a run (MakeWaterMark). An effect of its own: if it will not compile, the
    // water is drawn as it comes.
    TComRef<ID3DBlob> PsWaterMark;
    const char* const WaterMarkEntry = bStencilKinds ? "PSWaterMarkLit" : "PSWaterMark";
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, WaterMarkEntry, "ps_5_0", Flags, 0,
                          PsWaterMark.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("water mark pixel shader failed - where the water's waves stand before one another, the later "
                    "drawn shows: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        PsWaterMark.Release();
    }

    // The waterfall's own pixel shader (BLEND_Fall), over the same copies the water reads. An effect, not the picture: if
    // it will not compile, a fall is drawn modulated as it always was.
    TComRef<ID3DBlob> PsFall;
    const char* const FallEntry = bStencilKinds ? "PSFallLit" : "PSFall";
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, FallEntry, "ps_5_0", Flags, 0, PsFall.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("waterfall pixel shader failed - the falls stay modulated: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        PsFall.Release();
    }

    // The sludge's own pixel shaders (BLEND_Sludge, BLEND_SludgeFog): an opaque water surface on the water's lifted
    // patches, and its fog map over them. An effect too: if they will not compile, the sludge is drawn flat as it was.
    TComRef<ID3DBlob> PsSludge, PsSludgeFog;
    const char* const SludgeEntry    = bStencilKinds ? "PSSludgeLit" : "PSSludge";
    const char* const SludgeFogEntry = bStencilKinds ? "PSSludgeFogLit" : "PSSludgeFog";
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, SludgeEntry, "ps_5_0", Flags, 0, PsSludge.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, SludgeFogEntry, "ps_5_0", Flags, 0, PsSludgeFog.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("sludge pixel shaders failed - the opaque water is drawn flat: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        PsSludge.Release();
        PsSludgeFog.Release();
    }
    // And its mirror layer's (BLEND_SludgeMirror): an effect over the effect - without it the sludge has no mirror image,
    // as it had none before.
    TComRef<ID3DBlob> PsSludgeMirror;
    const char* const SludgeMirrorEntry = bStencilKinds ? "PSSludgeMirrorLit" : "PSSludgeMirror";
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, SludgeMirrorEntry, "ps_5_0", Flags, 0,
                          PsSludgeMirror.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("sludge mirror pixel shader failed - the opaque water has no mirror image: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        PsSludgeMirror.Release();
    }

    // The glass's mirror layer (BLEND_GlassMirror). An effect over the pane: if it will not compile, the levels' glass
    // has no mirror image, as it had none before.
    TComRef<ID3DBlob> PsGlassMirror;
    const char* const GlassMirrorEntry = bStencilKinds ? "PSGlassMirrorLit" : "PSGlassMirror";
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, GlassMirrorEntry, "ps_5_0", Flags, 0,
                          PsGlassMirror.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("glass mirror pixel shader failed - the levels' glass has no mirror image: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        PsGlassMirror.Release();
    }

    // An effect's body (BLEND_Body). An effect: if it will not compile, a mirror shows the game's own picture of a
    // flame, as it did before.
    TComRef<ID3DBlob> PsBody;
    const char* const BodyEntry = bStencilKinds ? "PSBodyLit" : "PSBody";
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, BodyEntry, "ps_5_0", Flags, 0,
                          PsBody.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("body pixel shader failed - a mirror shows the game's own flames: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        PsBody.Release();
    }

    // And the water's tessellation stages: its surface is lifted by its waves (VSWater, HSWater, DSWater).
    TComRef<ID3DBlob> VsWater, HsWater, DsWater;
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "VSWater", "vs_5_0", Flags, 0, VsWater.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "HSWater", "hs_5_0", Flags, 0, HsWater.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "DSWater", "ds_5_0", Flags, 0, DsWater.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("water tessellation shaders failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }

    // Geometry=Tessellation's stages: the relief made real (VSRelief, HSRelief, DSRelief). An experiment, so a failure
    // here costs only the experiment - logged, and the relief surfaces then carry the POM.
    TComRef<ID3DBlob> VsRelief, HsRelief, DsRelief;
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "VSRelief", "vs_5_0", Flags, 0, VsRelief.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "HSRelief", "hs_5_0", Flags, 0, HsRelief.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "DSRelief", "ds_5_0", Flags, 0, DsRelief.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("relief tessellation shaders failed - Geometry=DisplacementMapping draws as POM: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        VsRelief.Release();
        HsRelief.Release();
        DsRelief.Release();
    }
    TessPipelines[0].Release();
    TessPipelines[1].Release();
    TessModPipelines[0].Release();
    TessModPipelines[1].Release();
    TessFogPipelines[0].Release();
    TessFogPipelines[1].Release();
    GrassPipelines[0].Release();
    GrassPipelines[1].Release();
    TreePipelines[0].Release();
    TreePipelines[1].Release();
    DustPipelines[0].Release();
    DustPipelines[1].Release();

    D3D12_INPUT_ELEMENT_DESC Elements[5];
    appMemzero(Elements, sizeof(Elements));
    Elements[0].SemanticName      = "POSITION";
    Elements[0].Format            = DXGI_FORMAT_R32G32B32_FLOAT;
    Elements[0].AlignedByteOffset = 0;
    Elements[1].SemanticName      = "TEXCOORD";
    Elements[1].SemanticIndex     = 0;
    Elements[1].Format            = DXGI_FORMAT_R32G32_FLOAT;
    Elements[1].AlignedByteOffset = 12;
    Elements[2].SemanticName      = "TEXCOORD";
    Elements[2].SemanticIndex     = 1;
    Elements[2].Format            = DXGI_FORMAT_R32G32_FLOAT;
    Elements[2].AlignedByteOffset = 20;
    Elements[3].SemanticName      = "COLOR";
    Elements[3].Format            = DXGI_FORMAT_R32G32B32A32_FLOAT;
    Elements[4].SemanticName      = "TEXCOORD";
    Elements[4].SemanticIndex     = 2;
    Elements[4].Format            = DXGI_FORMAT_R32G32B32A32_FLOAT;
    Elements[4].AlignedByteOffset = 44;                 // the relief's own four, after the colour
    Elements[3].AlignedByteOffset = 28;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.pRootSignature     = RootSignature.Get();
    Desc.VS.pShaderBytecode = Vs->GetBufferPointer();
    Desc.VS.BytecodeLength  = Vs->GetBufferSize();
    Desc.PS.pShaderBytecode = Ps->GetBufferPointer();
    Desc.PS.BytecodeLength  = Ps->GetBufferSize();
    Desc.InputLayout.pInputElementDescs = Elements;
    Desc.InputLayout.NumElements        = 5;
    Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // With stencil kinds (the shadows) a second target, the light each surface receives, and a third, the added one
    // (PSMainLit). Each blend mode decides below what it writes to them; the targets' blends are independent.
    Desc.NumRenderTargets      = bStencilKinds ? 3 : 1;
    // The scene target's format, which the HDR key chooses (RdxColoursFor). It is NOT a constant: a pipeline
    // carries the format it writes, and one built for eight bits aimed at a ten-bit target writes eight-bit
    // numbers into ten-bit texels. Every surface then comes back with the bits of its neighbours mixed in - a
    // blue cast, a scrambled red channel and noise over what should be flat wall, which is exactly what it looked
    // like before this followed the mode.
    Desc.RTVFormats[0]         = Colours.Draw;
    if (bStencilKinds)
    {
        Desc.RTVFormats[1]                     = RDX_LIGHT_TARGET_FORMAT;
        Desc.RTVFormats[2]                     = Desc.RTVFormats[0];      // the added target: the picture's format
        Desc.BlendState.IndependentBlendEnable = TRUE;
    }
    Desc.DSVFormat             = bStencilKinds ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    Desc.SampleDesc.Count      = 1;
    Desc.SampleMask            = UINT_MAX;

    Desc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    // No back-face culling: the engine sends two-sided surfaces, and mirrored scene
    // nodes flip the winding (docs/spec-01/5.1). Culling would silently drop geometry.
    Desc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Desc.RasterizerState.DepthClipEnable = TRUE;

    // Reversed-Z with a float depth buffer: 1 is nearest, 0 the far plane, cleared to
    // 0. This puts the mantissa where the precision is needed for the 115..3000 unit
    // view depths the engine actually uses.
#if RDX_DEBUG_NODEPTH
    // Diagnostic build: no depth test at all. If geometry that is missing on screen
    // appears here, it was submitted and rejected; if it stays missing, it was never
    // submitted. Nothing else separates those two.
    Desc.DepthStencilState.DepthEnable = FALSE;
#else
    Desc.DepthStencilState.DepthEnable = TRUE;
#endif
    // GREATER_EQUAL, not GREATER: see RdxMath::DepthPasses. Every 2D tile arrives at the
    // same depth, and the engine expects the later one to cover the earlier one.
    Desc.DepthStencilState.DepthFunc   = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    // Stencil kinds (Init): the run's kind replaces the stencil wherever the draw passes the depth test - for the draws that
    // are the surface. A blended draw laid over one leaves the surface's kind (the write mask, per blend mode below).
    if (bStencilKinds)
    {
        D3D12_DEPTH_STENCILOP_DESC Op;
        Op.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
        Op.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        Op.StencilPassOp      = D3D12_STENCIL_OP_REPLACE;
        Op.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;
        Desc.DepthStencilState.StencilEnable    = TRUE;
        Desc.DepthStencilState.StencilReadMask  = 0xFF;
        Desc.DepthStencilState.StencilWriteMask = 0xFF;
        Desc.DepthStencilState.FrontFace        = Op;
        Desc.DepthStencilState.BackFace         = Op;
    }

    for (INT Mode = 0; Mode < BLEND_MAX; Mode++)
    {
        if (Mode == BLEND_Fall && !PsFall)
            continue;
        if ((Mode == BLEND_Sludge || Mode == BLEND_SludgeFog) && (!PsSludge || !PsSludgeFog))
        {
            for (INT Set = 0; Set < PIPE_SETS; Set++)
                Pipelines[Set][Mode].Release();
            continue;
        }
        if (Mode == BLEND_SludgeMirror && (!PsSludge || !PsSludgeFog || !PsSludgeMirror))
        {
            for (INT Set = 0; Set < PIPE_SETS; Set++)
            {
                Pipelines[Set][Mode].Release();
                RtHostSludgeMirrorPipelines[Set].Release();
                RtSludgeMirrorAskPipelines[Set].Release();
            }
            continue;
        }
        if (Mode == BLEND_Body && !PsBody)
        {
            for (INT Set = 0; Set < PIPE_SETS; Set++)
                Pipelines[Set][Mode].Release();
            continue;
        }
        if (Mode == BLEND_GlassMirror && !PsGlassMirror)
        {
            for (INT Set = 0; Set < PIPE_SETS; Set++)
            {
                Pipelines[Set][Mode].Release();
                RtGlassMirrorPipelines[Set].Release();
            }
            continue;
        }
        Desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;     // BLEND_FogEqual changes it
        D3D12_RENDER_TARGET_BLEND_DESC& B = Desc.BlendState.RenderTarget[0];
        appMemzero(&B, sizeof(B));
        B.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        B.BlendOp               = D3D12_BLEND_OP_ADD;
        B.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        B.SrcBlendAlpha         = D3D12_BLEND_ONE;
        B.DestBlendAlpha        = D3D12_BLEND_ZERO;

        switch (Mode)
        {
        case BLEND_Opaque:
            B.BlendEnable = FALSE;
            // Only opaque geometry writes depth. Blended surfaces must not occlude
            // what comes after them, and their draw order is the engine's, not ours.
            Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            break;

        case BLEND_Translucent:
            // src + dst * (1 - src). Black still adds nothing - a fade to black IS the
            // fade to invisible - but a bright source no longer piles fully on top of a
            // bright destination. Measured against the stock renderer on the HUD health
            // figure: with plain addition (ONE, ONE) its green parts came out about 12%
            // too bright before the output curve (torso 249 against its 228, legs 228
            // against 205), while their red and blue - where the source is near zero -
            // matched. Solving both readings for this formula gives a background of
            // 0.157, and the panel under the figure measures 0.144 on its own.
            // RDX_TRANSLUCENT_ADDITIVE=1 restores plain addition, for comparison.
#ifndef RDX_TRANSLUCENT_ADDITIVE
#define RDX_TRANSLUCENT_ADDITIVE 0
#endif
            B.BlendEnable = TRUE;
            B.SrcBlend    = D3D12_BLEND_ONE;
            B.DestBlend   = RDX_TRANSLUCENT_ADDITIVE ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_COLOR;
            Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            break;

        case BLEND_TranslucentOccluding:
            // The same blend, but it claims the depth buffer: the level marks the surface
            // PF_Occlude ("occludes even if PF_NoOcclude", SDK). See RdxMath::WritesDepth.
            B.BlendEnable = TRUE;
            B.SrcBlend    = D3D12_BLEND_ONE;
            B.DestBlend   = RDX_TRANSLUCENT_ADDITIVE ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_COLOR;
            Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            break;

        case BLEND_Modulated:
            // result = src * dst + dst * src = 2 * src * dst: 128 is neutral, and a brighter
            // texel really brightens. The factor of two has to live here, in the blend: the
            // shader's own output is clamped to 1 before blending (see the shader).
            B.BlendEnable = TRUE;
            B.SrcBlend    = D3D12_BLEND_DEST_COLOR;
            B.DestBlend   = D3D12_BLEND_SRC_COLOR;
            Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            break;

        case BLEND_OpaqueNoZWrite:
            // Opaque, but leaves the depth buffer alone. See RdxMath::WritesDepth.
            B.BlendEnable = FALSE;
            Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            break;

        case BLEND_Water:
        case BLEND_Fall:
            // The shader makes the whole result itself - the translucent blend over its own copy of what is behind - so
            // nothing is blended here; and like every blended surface it leaves the depth alone.
            B.BlendEnable = FALSE;
            Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            break;

        case BLEND_Sludge:
            // The surface itself, opaque: it writes the depth as the opaque draw does - what lies under it and is drawn
            // after it stays hidden - only its triangles are cut and lifted by the waves.
            B.BlendEnable = FALSE;
            Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            break;

        case BLEND_SludgeMirror:
        case BLEND_Body:
        case BLEND_GlassMirror:
            // rgb + dst * (1 - a): what the surface gives back over its own colour, by the sheen in alpha
            // (PSSludgeMirror, PSGlassMirror). The picture's alpha stays the surface's.
            B.BlendEnable    = TRUE;
            B.SrcBlend       = D3D12_BLEND_ONE;
            B.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
            B.SrcBlendAlpha  = D3D12_BLEND_ZERO;
            B.DestBlendAlpha = D3D12_BLEND_ONE;
            Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            break;

        case BLEND_SludgeFog:
        case BLEND_Fog:
        case BLEND_FogEqual:
            // fog + dst * (1 - fog's alpha): the engine's volumetric light as every renderer of it lays its fog maps.
            B.BlendEnable = TRUE;
            B.SrcBlend    = D3D12_BLEND_ONE;
            B.DestBlend   = D3D12_BLEND_INV_SRC_ALPHA;
            Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            if (Mode == BLEND_FogEqual)
                Desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
            break;
        }

        // The light target (stencil kinds only) is kept by the draws that write depth, so it belongs to the depth buffer's
        // surface - not by blended draws laid over that surface, nor by depthless tiles.
        if (bStencilKinds)
        {
            D3D12_RENDER_TARGET_BLEND_DESC& LB = Desc.BlendState.RenderTarget[1];
            appMemzero(&LB, sizeof(LB));
            LB.BlendEnable           = FALSE;
            LB.SrcBlend              = D3D12_BLEND_ONE;
            LB.DestBlend             = D3D12_BLEND_ZERO;
            LB.BlendOp               = D3D12_BLEND_OP_ADD;
            LB.SrcBlendAlpha         = D3D12_BLEND_ONE;
            LB.DestBlendAlpha        = D3D12_BLEND_ZERO;
            LB.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            LB.RenderTargetWriteMask = (Mode == BLEND_Opaque || Mode == BLEND_TranslucentOccluding || Mode == BLEND_Sludge)
                                     ? (UINT8)D3D12_COLOR_WRITE_ENABLE_ALL : (UINT8)0;

            // The added target: blended as the picture is. A draw that is the surface - opaque, or translucent claiming the
            // depth - starts it anew: its own picture is what the shadows darken, all but the flashlight's light on it,
            // which its shader hands over alone (PSMainLitSurface). The sludge takes no flashlight, and clears it.
            D3D12_RENDER_TARGET_BLEND_DESC& AB = Desc.BlendState.RenderTarget[2];
            AB = B;
            if (Mode == BLEND_Opaque || Mode == BLEND_OpaqueNoZWrite || Mode == BLEND_TranslucentOccluding)
                AB.BlendEnable = FALSE;
            else if (Mode == BLEND_Sludge)
            {
                AB.BlendEnable = TRUE;
                AB.SrcBlend    = D3D12_BLEND_ZERO;
                AB.DestBlend   = D3D12_BLEND_ZERO;
            }
            else if (Mode == BLEND_Water || Mode == BLEND_Fall)
            {
                // The larger of the surface's own share and what was there - not laid over it. The shader makes the
                // picture whole from its copy of what is behind: where two sheets cover the same pixel (lifted by
                // their waves, they overlap along the edge they share) the picture has the later one's alone, and
                // a share laid over the earlier one's stood out of every shadow as a bright seam.
                AB.BlendEnable = TRUE;
                AB.SrcBlend    = D3D12_BLEND_ONE;
                AB.DestBlend   = D3D12_BLEND_ONE;
                AB.BlendOp     = D3D12_BLEND_OP_MAX;
            }
            else if (bAddedTranslucentOff)
                AB.RenderTargetWriteMask = 0;      // the measurement's marker (SetAddedTranslucentOff)
            Desc.DepthStencilState.StencilWriteMask =
                (Mode == BLEND_Translucent || Mode == BLEND_Modulated || Mode == BLEND_Water || Mode == BLEND_Fog ||
                 Mode == BLEND_FogEqual || Mode == BLEND_Fall || Mode == BLEND_SludgeFog ||
                 Mode == BLEND_SludgeMirror || Mode == BLEND_GlassMirror || Mode == BLEND_Body)
                ? 0x00 : 0xFF;
        }

        // Modulated draws get the doubling shader; everything else the ordinary one.
        if (Mode == BLEND_Modulated)
        {
            Desc.PS.pShaderBytecode = PsMod->GetBufferPointer();
            Desc.PS.BytecodeLength  = PsMod->GetBufferSize();
        }
        else if (Mode == BLEND_Fog || Mode == BLEND_FogEqual)
        {
            Desc.PS.pShaderBytecode = PsFog->GetBufferPointer();
            Desc.PS.BytecodeLength  = PsFog->GetBufferSize();
        }
        else if (Mode == BLEND_Fall)
        {
            Desc.PS.pShaderBytecode = PsFall->GetBufferPointer();
            Desc.PS.BytecodeLength  = PsFall->GetBufferSize();
        }
        else if (Mode == BLEND_GlassMirror)
        {
            Desc.PS.pShaderBytecode = PsGlassMirror->GetBufferPointer();
            Desc.PS.BytecodeLength  = PsGlassMirror->GetBufferSize();
        }
        else if (Mode == BLEND_Body)
        {
            Desc.PS.pShaderBytecode = PsBody->GetBufferPointer();
            Desc.PS.BytecodeLength  = PsBody->GetBufferSize();
        }
        else if (Mode == BLEND_Water || Mode == BLEND_Sludge || Mode == BLEND_SludgeFog || Mode == BLEND_SludgeMirror)
        {
            ID3DBlob* const WaterPs = (Mode == BLEND_Water)     ? PsWater.Get()
                                    : (Mode == BLEND_Sludge)    ? PsSludge.Get()
                                    : (Mode == BLEND_SludgeFog) ? PsSludgeFog.Get() : PsSludgeMirror.Get();
            Desc.PS.pShaderBytecode = WaterPs->GetBufferPointer();
            Desc.PS.BytecodeLength  = WaterPs->GetBufferSize();
            Desc.VS.pShaderBytecode = VsWater->GetBufferPointer();
            Desc.VS.BytecodeLength  = VsWater->GetBufferSize();
            Desc.HS.pShaderBytecode = HsWater->GetBufferPointer();
            Desc.HS.BytecodeLength  = HsWater->GetBufferSize();
            Desc.DS.pShaderBytecode = DsWater->GetBufferPointer();
            Desc.DS.BytecodeLength  = DsWater->GetBufferSize();
            Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        }
        else
        {
            // The surface's own entry point goes with the added target's blend above: the same three modes.
            ID3DBlob* const Own = (PsSurface && (Mode == BLEND_Opaque || Mode == BLEND_OpaqueNoZWrite ||
                                                 Mode == BLEND_TranslucentOccluding)) ? PsSurface.Get() : Ps.Get();
            Desc.PS.pShaderBytecode = Own->GetBufferPointer();
            Desc.PS.BytecodeLength  = Own->GetBufferSize();
        }

        const HRESULT Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState), Pipelines[0][Mode].VoidSlot());
        // And the same pipeline for the multisampled world. Only the sample count differs - every blend, every mask
        // and every format stays, or the two sets would draw two different pictures.
        HRESULT MadeMs = S_OK;
        if (SUCCEEDED(Made) && MsaaSamples > 1)
        {
            Desc.SampleDesc.Count = (UINT)MsaaSamples;
            MadeMs = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState), Pipelines[1][Mode].VoidSlot());
            Desc.SampleDesc.Count = 1;
        }
        if (Mode == BLEND_Opaque && SUCCEEDED(Made) && SUCCEEDED(MadeMs))
        {
            MakeGrass(Device, Desc);
            MakeTree(Device, Desc);
        }
        if (Mode == BLEND_Translucent && SUCCEEDED(Made) && SUCCEEDED(MadeMs))
            MakeDust(Device, Desc);
        // Reflections=RT: the same pipeline, its stages from the DXIL - the water's, and the sludge's mirror layer's.
        if (Mode == BLEND_Water && SUCCEEDED(Made) && SUCCEEDED(MadeMs))
        {
            MakeRtWater(Device, Desc);
            MakeWaterMark(Device, Desc, PsWaterMark.Get());
        }
        if (Mode == BLEND_SludgeMirror && SUCCEEDED(Made) && SUCCEEDED(MadeMs))
            MakeRtSludgeMirror(Device, Desc);
        if (Mode == BLEND_GlassMirror && SUCCEEDED(Made) && SUCCEEDED(MadeMs))
            MakeRtGlassMirror(Device, Desc);
        // Geometry=Tessellation: the opaque pipeline once more, with the relief's stages in front of the same pixel
        // shader - so it writes depth, stencil kind and the light target exactly as the flat surface does - and the
        // modulated one, for the detail texture laid over the same patches.
        if ((Mode == BLEND_Opaque || Mode == BLEND_Modulated || Mode == BLEND_Fog) && SUCCEEDED(Made) &&
            SUCCEEDED(MadeMs) && VsRelief && HsRelief && DsRelief)
        {
            TComRef<ID3D12PipelineState>* const Set = (Mode == BLEND_Opaque)    ? TessPipelines
                                                    : (Mode == BLEND_Modulated) ? TessModPipelines
                                                                                : TessFogPipelines;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC T = Desc;
            T.VS.pShaderBytecode  = VsRelief->GetBufferPointer();
            T.VS.BytecodeLength   = VsRelief->GetBufferSize();
            T.HS.pShaderBytecode  = HsRelief->GetBufferPointer();
            T.HS.BytecodeLength   = HsRelief->GetBufferSize();
            T.DS.pShaderBytecode  = DsRelief->GetBufferPointer();
            T.DS.BytecodeLength   = DsRelief->GetBufferSize();
            T.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            T.SampleDesc.Count    = 1;
            HRESULT TessMade = Device->CreateGraphicsPipelineState(&T, __uuidof(ID3D12PipelineState), Set[0].VoidSlot());
            if (SUCCEEDED(TessMade) && MsaaSamples > 1)
            {
                T.SampleDesc.Count = (UINT)MsaaSamples;
                TessMade = Device->CreateGraphicsPipelineState(&T, __uuidof(ID3D12PipelineState), Set[1].VoidSlot());
            }
            if (FAILED(TessMade))
            {
                RdxLog(TEXT("relief tessellation pipeline (%s) refused (0x%08X) - %s"),
                       (Mode == BLEND_Opaque) ? TEXT("the surface") : TEXT("the detail texture"), (DWORD)TessMade,
                       (Mode == BLEND_Opaque) ? TEXT("Geometry=DisplacementMapping draws as POM")
                                              : TEXT("the relief goes without its detail texture"));
                Set[0].Release();
                Set[1].Release();
            }
        }
        // The water's stages belong to the water alone.
        Desc.VS.pShaderBytecode    = Vs->GetBufferPointer();
        Desc.VS.BytecodeLength     = Vs->GetBufferSize();
        Desc.HS.pShaderBytecode    = nullptr;
        Desc.HS.BytecodeLength     = 0;
        Desc.DS.pShaderBytecode    = nullptr;
        Desc.DS.BytecodeLength     = 0;
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        // Its mirror layer is an effect over it: refused, the sludge simply has no mirror image (CanSludgeMirror).
        if (Mode == BLEND_SludgeMirror && (FAILED(Made) || FAILED(MadeMs)))
        {
            RdxLog(TEXT("sludge mirror pipeline refused (0x%08X) - the opaque water has no mirror image"),
                   (DWORD)(FAILED(Made) ? Made : MadeMs));
            for (INT Set = 0; Set < PIPE_SETS; Set++)
            {
                Pipelines[Set][BLEND_SludgeMirror].Release();
            }
            continue;
        }
        // So is the glass's: refused, the levels' glass has no mirror image (CanGlassMirror).
        if (Mode == BLEND_GlassMirror && (FAILED(Made) || FAILED(MadeMs)))
        {
            RdxLog(TEXT("glass mirror pipeline refused (0x%08X) - the levels' glass has no mirror image"),
                   (DWORD)(FAILED(Made) ? Made : MadeMs));
            for (INT Set = 0; Set < PIPE_SETS; Set++)
            {
                Pipelines[Set][BLEND_GlassMirror].Release();
                RtGlassMirrorPipelines[Set].Release();
            }
            continue;
        }
        // The sludge is an effect: a pipeline refused leaves it drawn flat (CanSludge), as it always was.
        if ((Mode == BLEND_Sludge || Mode == BLEND_SludgeFog) && (FAILED(Made) || FAILED(MadeMs)))
        {
            RdxLog(TEXT("sludge pipeline %i refused (0x%08X) - the opaque water is drawn flat"), Mode,
                   (DWORD)(FAILED(Made) ? Made : MadeMs));
            for (INT Set = 0; Set < PIPE_SETS; Set++)
            {
                Pipelines[Set][BLEND_Sludge].Release();
                Pipelines[Set][BLEND_SludgeFog].Release();
            }
            continue;
        }
        if (FAILED(Made))
        {
            RdxLog(TEXT("pipeline state %i creation failed"), Mode);
            return 0;
        }
        if (FAILED(MadeMs))
        {
            RdxLog(TEXT("MSAA: pipeline %i could not be created with %i samples - MSAA is skipped"), Mode, MsaaSamples);
            return 0;
        }
    }

    Desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;     // the last mode left EQUAL in it
    // The relief's covers (DrawReliefCovers): the world's surfaces again, depth alone - no target and no stencil, the masked
    // texels discarded as the picture discards them (PSCover). Single-sample: under MSAA there are no covers. Without it
    // the relief is raised through everything, as it was before the covers.
    CoverPipeline.Release();
    {
        TComRef<ID3DBlob> CoverPs;
        if (SUCCEEDED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "PSCover", "ps_5_0", Flags, 0,
                                 CoverPs.Slot(), Error.Slot())))
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC C = Desc;
            C.PS.pShaderBytecode = CoverPs->GetBufferPointer();
            C.PS.BytecodeLength  = CoverPs->GetBufferSize();
            C.NumRenderTargets   = 0;
            for (INT t = 0; t < 8; t++)
                C.RTVFormats[t] = DXGI_FORMAT_UNKNOWN;
            appMemzero(&C.BlendState, sizeof(C.BlendState));
            C.DepthStencilState.DepthEnable    = TRUE;
            C.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            C.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            C.DepthStencilState.StencilEnable  = FALSE;
            C.SampleDesc.Count                 = 1;
            if (FAILED(Device->CreateGraphicsPipelineState(&C, __uuidof(ID3D12PipelineState), CoverPipeline.VoidSlot())))
                CoverPipeline.Release();
        }
        if (!CoverPipeline)
            RdxLog(TEXT("relief: the covers' pipeline could not be made - the relief is raised through everything"));
    }

    // The screen tint (EndFlash): dst * FlashScale + FlashFog over the whole target. The
    // scale is the blend factor, set per tint run; the fog is the shader's output colour.
    TComRef<ID3DBlob> FlashVs, FlashPs;
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "FlashVS", "vs_5_0", Flags, 0, FlashVs.Slot(), Error.Slot())) ||
        FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, "FlashPS", "ps_5_0", Flags, 0, FlashPs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("screen tint shader failed: %s"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Tint = Desc;
    Tint.VS.pShaderBytecode = FlashVs->GetBufferPointer();
    Tint.VS.BytecodeLength  = FlashVs->GetBufferSize();
    Tint.PS.pShaderBytecode = FlashPs->GetBufferPointer();
    Tint.PS.BytecodeLength  = FlashPs->GetBufferSize();
    Tint.InputLayout.pInputElementDescs = nullptr;
    Tint.InputLayout.NumElements        = 0;
    Tint.DepthStencilState.DepthEnable    = FALSE;
    Tint.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    Tint.DepthStencilState.StencilEnable  = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& TB = Tint.BlendState.RenderTarget[0];
    appMemzero(&TB, sizeof(TB));
    TB.BlendEnable           = TRUE;
    TB.SrcBlend              = D3D12_BLEND_ONE;
    TB.DestBlend             = D3D12_BLEND_BLEND_FACTOR;
    TB.BlendOp               = D3D12_BLEND_OP_ADD;
    TB.SrcBlendAlpha         = D3D12_BLEND_ZERO;
    TB.DestBlendAlpha        = D3D12_BLEND_ONE;
    TB.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    TB.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // The tint leaves the light target alone, and goes into the added target as into the picture (stencil kinds only - Tint
    // copied the three-target description).
    if (bStencilKinds)
    {
        Tint.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
        Tint.BlendState.RenderTarget[2]                       = TB;
    }
    if (FAILED(Device->CreateGraphicsPipelineState(&Tint, __uuidof(ID3D12PipelineState), FlashPipeline[0].VoidSlot())))
    {
        RdxLog(TEXT("screen tint pipeline creation failed"));
        return 0;
    }
    if (MsaaSamples > 1)
    {
        Tint.SampleDesc.Count = (UINT)MsaaSamples;
        if (FAILED(Device->CreateGraphicsPipelineState(&Tint, __uuidof(ID3D12PipelineState), FlashPipeline[1].VoidSlot())))
        {
            RdxLog(TEXT("MSAA: the screen tint pipeline could not be created with %i samples"), MsaaSamples);
            return 0;
        }
    }

    // The water's splashes laid on (FBackend::DrawSplashes, PSSplash): the tint's one triangle, their colour over the
    // picture by their coverage - and into the added target alike, so the shadows leave them alone. Without it the water
    // simply has no splashes; nothing else depends on it.
    TComRef<ID3DBlob> SplashPs;
    if (FAILED(RdxCompile(GSolidPassText, Len, "GeometryPass", Macros, nullptr, bStencilKinds ? "PSSplashLit" : "PSSplash",
                          "ps_5_0", Flags, 0, SplashPs.Slot(), Error.Slot())))
    {
        RdxLog(TEXT("water: the splashes' shader failed: %s - no splashes"),
               Error ? appFromAnsi((const ANSICHAR*)Error->GetBufferPointer()) : TEXT("(no message)"));
        return 1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Lay = Tint;
    Lay.PS.pShaderBytecode = SplashPs->GetBufferPointer();
    Lay.PS.BytecodeLength  = SplashPs->GetBufferSize();
    Lay.SampleDesc.Count   = 1;
    D3D12_RENDER_TARGET_BLEND_DESC& LB = Lay.BlendState.RenderTarget[0];
    appMemzero(&LB, sizeof(LB));
    LB.BlendEnable           = TRUE;
    LB.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    LB.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    LB.BlendOp               = D3D12_BLEND_OP_ADD;
    LB.SrcBlendAlpha         = D3D12_BLEND_ZERO;
    LB.DestBlendAlpha        = D3D12_BLEND_ONE;
    LB.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    LB.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (bStencilKinds)
    {
        Lay.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
        Lay.BlendState.RenderTarget[2]                       = LB;
    }
    if (FAILED(Device->CreateGraphicsPipelineState(&Lay, __uuidof(ID3D12PipelineState), SplashPipeline[0].VoidSlot())))
    {
        RdxLog(TEXT("water: the splashes' pipeline could not be created - no splashes"));
        return 1;
    }
    if (MsaaSamples > 1)
    {
        Lay.SampleDesc.Count = (UINT)MsaaSamples;
        if (FAILED(Device->CreateGraphicsPipelineState(&Lay, __uuidof(ID3D12PipelineState), SplashPipeline[1].VoidSlot())))
            RdxLog(TEXT("MSAA: the splashes' pipeline could not be created with %i samples - no splashes with MSAA"),
                   MsaaSamples);
    }
    // On rays (Reflections=RT) a fountain's falling water mirrors the room. The rays are the 64-bit host's
    // (SetRtHosted): the same triangle with the build that reads the host's answers, and with the one that writes its
    // pixels' rays down - both its stages as DXIL (a pipeline may not mix the two kinds of shader), on the rays' root
    // signature. Without them the water is laid on as above, and mirrors what the picture holds around it.
    RtHostSplashPipeline[0].Release();
    RtHostSplashPipeline[1].Release();
    RtSplashAskPipeline[0].Release();
    RtSplashAskPipeline[1].Release();
    if (bRtHosted && RtWaterRoot)
    {
        Lay.pRootSignature     = RtWaterRoot.Get();
        Lay.VS.pShaderBytecode = GSolidPassRtFlashVS;
        Lay.VS.BytecodeLength  = sizeof(GSolidPassRtFlashVS);
        MakeHostPair(Device, Lay, bStencilKinds ? (const void*)GSolidPassRtHostSplashPSLit : GSolidPassRtHostSplashPS,
                     bStencilKinds ? sizeof(GSolidPassRtHostSplashPSLit) : sizeof(GSolidPassRtHostSplashPS),
                     GSolidPassRtAskSplashPS, sizeof(GSolidPassRtAskSplashPS), RtHostSplashPipeline,
                     RtSplashAskPipeline, TEXT("the splashes'"));
    }
    return 1;
}

void FSolidPass::MakeHostPair(ID3D12Device* Device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Layer, const void* HostPs,
                              SIZE_T HostBytes, const void* AskPs, SIZE_T AskBytes, TComRef<ID3D12PipelineState>* Host,
                              TComRef<ID3D12PipelineState>* Ask, const TCHAR* What)
{
    for (INT Set = 0; Set < PIPE_SETS; Set++)
    {
        Host[Set].Release();
        Ask[Set].Release();
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Reads = Layer;
    Reads.PS.pShaderBytecode = HostPs;
    Reads.PS.BytecodeLength  = HostBytes;
    Reads.SampleDesc.Count   = 1;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Asks = Layer;
    Asks.PS.pShaderBytecode = AskPs;
    Asks.PS.BytecodeLength  = AskBytes;
    Asks.NumRenderTargets   = 0;
    for (INT k = 0; k < 8; k++)
        Asks.RTVFormats[k] = DXGI_FORMAT_UNKNOWN;
    appMemzero(&Asks.BlendState, sizeof(Asks.BlendState));
    Asks.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    Asks.DepthStencilState.StencilEnable  = FALSE;
    Asks.SampleDesc.Count   = 1;
    HRESULT Made = Device->CreateGraphicsPipelineState(&Reads, __uuidof(ID3D12PipelineState), Host[0].VoidSlot());
    if (SUCCEEDED(Made))
        Made = Device->CreateGraphicsPipelineState(&Asks, __uuidof(ID3D12PipelineState), Ask[0].VoidSlot());
    if (SUCCEEDED(Made) && MsaaSamples > 1)
    {
        Reads.SampleDesc.Count = Asks.SampleDesc.Count = (UINT)MsaaSamples;
        Made = Device->CreateGraphicsPipelineState(&Reads, __uuidof(ID3D12PipelineState), Host[1].VoidSlot());
        if (SUCCEEDED(Made))
            Made = Device->CreateGraphicsPipelineState(&Asks, __uuidof(ID3D12PipelineState), Ask[1].VoidSlot());
    }
    if (FAILED(Made))
    {
        for (INT Set = 0; Set < PIPE_SETS; Set++)
        {
            Host[Set].Release();
            Ask[Set].Release();
        }
        RdxLog(TEXT("rays (host): %s pipelines for the 64-bit host's rays were refused (0x%08X) - through the host ")
               TEXT("they are drawn as without rays"), What, (UINT)Made);
    }
}

// The sample count changed in the running game: throw both sets away and build them again. Between frames only -
// the caller (FBackend::ApplyMsaaCount) is the one that knows that.
void FSolidPass::MakeRtWater(ID3D12Device* Device, D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc)
{
    for (INT Set = 0; Set < PIPE_SETS; Set++)
    {
        RtHostWaterPipelines[Set].Release();
        RtWaterAskPipelines[Set].Release();
    }
    if (!bRtHosted || !RtWaterRoot)
        return;
    Desc.pRootSignature     = RtWaterRoot.Get();
    Desc.VS.pShaderBytecode = GSolidPassRtVS;
    Desc.VS.BytecodeLength  = sizeof(GSolidPassRtVS);
    Desc.HS.pShaderBytecode = GSolidPassRtHS;
    Desc.HS.BytecodeLength  = sizeof(GSolidPassRtHS);
    Desc.DS.pShaderBytecode = GSolidPassRtDS;
    Desc.DS.BytecodeLength  = sizeof(GSolidPassRtDS);
    // The run's build that reads the host's answers, and the one that writes its rays down.
    MakeHostPair(Device, Desc, bStencilKinds ? (const void*)GSolidPassRtHostPSLit : GSolidPassRtHostPS,
                 bStencilKinds ? sizeof(GSolidPassRtHostPSLit) : sizeof(GSolidPassRtHostPS), GSolidPassRtAskPS,
                 sizeof(GSolidPassRtAskPS), RtHostWaterPipelines, RtWaterAskPipelines, TEXT("the water's"));
}

// The see-through water's first go over a run: the water's own description with a pixel shader that only marks the
// nearest of the surface's points in the water's own depth (PSWaterMark), every target masked. And the same for the RT
// water, with the stages its drawing has - the two goes have to put every point in the very same place, and the
// run-time compiled stages and the DXIL ones need not agree to the last bit. An effect: refused, the water is drawn
// as it comes.
void FSolidPass::MakeWaterMark(ID3D12Device* Device, D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc, ID3DBlob* Ps)
{
    for (INT Set = 0; Set < PIPE_SETS; Set++)
    {
        WaterMarkPipelines[Set].Release();
        RtWaterMarkPipelines[Set].Release();
        RtWaterAskMarkPipelines[Set].Release();
    }
    if (!Ps)
        return;
    for (INT k = 0; k < 8; k++)
        Desc.BlendState.RenderTarget[k].RenderTargetWriteMask = 0;
    Desc.SampleDesc.Count   = 1;
    Desc.PS.pShaderBytecode = Ps->GetBufferPointer();
    Desc.PS.BytecodeLength  = Ps->GetBufferSize();
    HRESULT Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                       WaterMarkPipelines[0].VoidSlot());
    if (SUCCEEDED(Made) && MsaaSamples > 1)
    {
        Desc.SampleDesc.Count = (UINT)MsaaSamples;
        Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                   WaterMarkPipelines[1].VoidSlot());
        Desc.SampleDesc.Count = 1;
    }
    if (FAILED(Made))
    {
        WaterMarkPipelines[0].Release();
        WaterMarkPipelines[1].Release();
        RdxLog(TEXT("water: the pipeline that marks its nearest points was refused (0x%08X) - where its waves stand "
                    "before one another, the later drawn shows"), (UINT)Made);
        return;
    }
    if (!RtWaterRoot || !RtHostWaterPipelines[0])
        return;
    Desc.pRootSignature     = RtWaterRoot.Get();
    Desc.VS.pShaderBytecode = GSolidPassRtVS;
    Desc.VS.BytecodeLength  = sizeof(GSolidPassRtVS);
    Desc.HS.pShaderBytecode = GSolidPassRtHS;
    Desc.HS.BytecodeLength  = sizeof(GSolidPassRtHS);
    Desc.DS.pShaderBytecode = GSolidPassRtDS;
    Desc.DS.BytecodeLength  = sizeof(GSolidPassRtDS);
    Desc.PS.pShaderBytecode = bStencilKinds ? GSolidPassRtMarkPSLit : GSolidPassRtMarkPS;
    Desc.PS.BytecodeLength  = bStencilKinds ? sizeof(GSolidPassRtMarkPSLit) : sizeof(GSolidPassRtMarkPS);
    Desc.SampleDesc.Count   = 1;
    Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                               RtWaterMarkPipelines[0].VoidSlot());
    if (SUCCEEDED(Made) && MsaaSamples > 1)
    {
        Desc.SampleDesc.Count = (UINT)MsaaSamples;
        Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                   RtWaterMarkPipelines[1].VoidSlot());
    }
    if (FAILED(Made))
    {
        RtWaterMarkPipelines[0].Release();
        RtWaterMarkPipelines[1].Release();
        RdxLog(TEXT("water: the RT water's pipeline that marks its nearest points was refused (0x%08X) - where its "
                    "waves stand before one another, the later drawn shows"), (UINT)Made);
        return;
    }
    // The same go before a run's rays are written down for the 64-bit host: with no target, as the asking has
    // none. Refused, a run asks for every one of its points, and the drawing still keeps the nearest alone.
    if (!RtWaterAskPipelines[0])
        return;
    Desc.PS.pShaderBytecode = GSolidPassRtAskMarkPS;
    Desc.PS.BytecodeLength  = sizeof(GSolidPassRtAskMarkPS);
    Desc.NumRenderTargets   = 0;
    for (INT k = 0; k < 8; k++)
        Desc.RTVFormats[k] = DXGI_FORMAT_UNKNOWN;
    appMemzero(&Desc.BlendState, sizeof(Desc.BlendState));
    Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    Desc.DepthStencilState.StencilEnable  = FALSE;
    Desc.SampleDesc.Count   = 1;
    Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                               RtWaterAskMarkPipelines[0].VoidSlot());
    if (SUCCEEDED(Made) && MsaaSamples > 1)
    {
        Desc.SampleDesc.Count = (UINT)MsaaSamples;
        Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                   RtWaterAskMarkPipelines[1].VoidSlot());
    }
    if (FAILED(Made))
    {
        RtWaterAskMarkPipelines[0].Release();
        RtWaterAskMarkPipelines[1].Release();
        RdxLog(TEXT("water: the pipeline that marks a run's nearest points before its rays are asked of the 64-bit ")
               TEXT("host was refused (0x%08X) - every point of the run asks"), (UINT)Made);
    }
}

// The sludge's mirror layer on Reflections=RT: its own description with the RT water's stages and root signature, and
// the layer's pixel shader as DXIL. The surface under it is drawn with the run-time compiled stages, these from the
// DXIL - the same arithmetic by two compilers, which need not agree to the last bit - so the layer is drawn a hair
// nearer, or the depth test would let parts of it fall behind the very surface it lies on.
void FSolidPass::MakeRtSludgeMirror(ID3D12Device* Device, D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc)
{
    for (INT Set = 0; Set < PIPE_SETS; Set++)
    {
        RtHostSludgeMirrorPipelines[Set].Release();
        RtSludgeMirrorAskPipelines[Set].Release();
    }
    if (!bRtHosted || !RtWaterRoot)
        return;
    Desc.pRootSignature                      = RtWaterRoot.Get();
    Desc.VS.pShaderBytecode                  = GSolidPassRtVS;
    Desc.VS.BytecodeLength                   = sizeof(GSolidPassRtVS);
    Desc.HS.pShaderBytecode                  = GSolidPassRtHS;
    Desc.HS.BytecodeLength                   = sizeof(GSolidPassRtHS);
    Desc.DS.pShaderBytecode                  = GSolidPassRtDS;
    Desc.DS.BytecodeLength                   = sizeof(GSolidPassRtDS);
    Desc.RasterizerState.DepthBias            = 16;        // reversed-Z: towards the camera
    Desc.RasterizerState.SlopeScaledDepthBias = 1.0f;
    // The layer's build that reads the host's answers, and the one that writes its rays down - drawn a hair nearer
    // as the layer is.
    MakeHostPair(Device, Desc,
                 bStencilKinds ? (const void*)GSolidPassRtHostSludgeMirrorPSLit : GSolidPassRtHostSludgeMirrorPS,
                 bStencilKinds ? sizeof(GSolidPassRtHostSludgeMirrorPSLit) : sizeof(GSolidPassRtHostSludgeMirrorPS),
                 GSolidPassRtAskSludgeMirrorPS, sizeof(GSolidPassRtAskSludgeMirrorPS),
                 RtHostSludgeMirrorPipelines, RtSludgeMirrorAskPipelines, TEXT("the sludge mirror layer's"));
}

// The glass's mirror layer on Reflections=RT: its own description with the rays' root signature, the pass's own
// vertex shader as DXIL - a pipeline's stages are all of one kind - and the layer's pixel shader as DXIL. The pane
// under it writes no depth, so the two compilers' arithmetic need not agree to the last bit here.
void FSolidPass::MakeRtGlassMirror(ID3D12Device* Device, D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc)
{
    RtGlassMirrorPipelines[0].Release();
    RtGlassMirrorPipelines[1].Release();
    RtHostPaneKeepPipeline.Release();
    RtPaneKeepAskPipeline.Release();
    RtGlassAskPipelines[0].Release();
    RtGlassAskPipelines[1].Release();
    if (!bRtHosted || !RtWaterRoot)
        return;
    Desc.pRootSignature     = RtWaterRoot.Get();
    Desc.VS.pShaderBytecode = GSolidPassRtMainVS;
    Desc.VS.BytecodeLength  = sizeof(GSolidPassRtMainVS);
    // The layer as it is drawn, for its second build further down: the description is changed on the way there.
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC Layer = Desc;
    // The build of the layer that reads the host's answers (RDX_RT_HOST).
    Desc.PS.pShaderBytecode = bStencilKinds ? GSolidPassRtHostGlassMirrorPSLit : GSolidPassRtHostGlassMirrorPS;
    Desc.PS.BytecodeLength  = bStencilKinds ? sizeof(GSolidPassRtHostGlassMirrorPSLit)
                                            : sizeof(GSolidPassRtHostGlassMirrorPS);
    Desc.SampleDesc.Count   = 1;
    HRESULT Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                       RtGlassMirrorPipelines[0].VoidSlot());
    if (SUCCEEDED(Made) && MsaaSamples > 1)
    {
        Desc.SampleDesc.Count = (UINT)MsaaSamples;
        Made = Device->CreateGraphicsPipelineState(&Desc, __uuidof(ID3D12PipelineState),
                                                   RtGlassMirrorPipelines[1].VoidSlot());
    }
    if (FAILED(Made))
    {
        RtGlassMirrorPipelines[0].Release();
        RtGlassMirrorPipelines[1].Release();
        RdxLog(TEXT("glass: the mirror layer's pipeline on rays was refused (0x%08X) - the panes give nothing back"),
               (UINT)Made);
        return;
    }
    // The panes drawn once more with targets of their own - the same stages before the pixel shader, nothing blended
    // and no depth.
    Desc.SampleDesc.Count   = 1;
    for (INT k = 0; k < 8; k++)
        Desc.RTVFormats[k] = DXGI_FORMAT_UNKNOWN;
    appMemzero(&Desc.BlendState, sizeof(Desc.BlendState));
    Desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    appMemzero(&Desc.DepthStencilState, sizeof(Desc.DepthStencilState));
    Desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    // Into the mirrors' map (PSPaneKeep), one target of the map's format, reading the host's answers - and before
    // that with no target at all, their rays written down. Both or neither.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC Keep = Desc;
        Keep.PS.pShaderBytecode = GSolidPassRtHostPaneKeepPS;
        Keep.PS.BytecodeLength  = sizeof(GSolidPassRtHostPaneKeepPS);
        Keep.NumRenderTargets   = 1;
        Keep.RTVFormats[0]      = DXGI_FORMAT_R32G32_FLOAT;
        HRESULT Kept = Device->CreateGraphicsPipelineState(&Keep, __uuidof(ID3D12PipelineState),
                                                           RtHostPaneKeepPipeline.VoidSlot());
        Keep.PS.pShaderBytecode = GSolidPassRtAskPaneKeepPS;
        Keep.PS.BytecodeLength  = sizeof(GSolidPassRtAskPaneKeepPS);
        Keep.NumRenderTargets   = 0;
        Keep.RTVFormats[0]      = DXGI_FORMAT_UNKNOWN;
        appMemzero(&Keep.BlendState, sizeof(Keep.BlendState));
        if (SUCCEEDED(Kept))
            Kept = Device->CreateGraphicsPipelineState(&Keep, __uuidof(ID3D12PipelineState),
                                                       RtPaneKeepAskPipeline.VoidSlot());
        if (FAILED(Kept))
        {
            RtHostPaneKeepPipeline.Release();
            RtPaneKeepAskPipeline.Release();
            RdxLog(TEXT("glass: the panes' pipelines for the mirrors' map on the 64-bit host's rays were refused ")
                   TEXT("(0x%08X) - through the host no flashlight's light off glass"), (UINT)Kept);
        }
    }
    // The layer's rays written down for the host (PSGlassMirrorAsk), into the rays' list. The layer as it is
    // drawn - the same stages before the pixel shader, the same test against the world's depth, so that a pixel the
    // picture hides asks nothing - with no target at all: what the shader writes, it writes into the list. Without
    // it the layer has nothing to read: no layer.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Ask = Layer;
    Ask.PS.pShaderBytecode = GSolidPassRtAskGlassMirrorPS;
    Ask.PS.BytecodeLength  = sizeof(GSolidPassRtAskGlassMirrorPS);
    Ask.NumRenderTargets   = 0;
    for (INT k = 0; k < 8; k++)
        Ask.RTVFormats[k] = DXGI_FORMAT_UNKNOWN;
    appMemzero(&Ask.BlendState, sizeof(Ask.BlendState));
    Ask.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    Ask.DepthStencilState.StencilEnable  = FALSE;
    Ask.SampleDesc.Count   = 1;
    HRESULT Asked = Device->CreateGraphicsPipelineState(&Ask, __uuidof(ID3D12PipelineState),
                                                        RtGlassAskPipelines[0].VoidSlot());
    if (SUCCEEDED(Asked) && MsaaSamples > 1)
    {
        Ask.SampleDesc.Count = (UINT)MsaaSamples;
        Asked = Device->CreateGraphicsPipelineState(&Ask, __uuidof(ID3D12PipelineState),
                                                    RtGlassAskPipelines[1].VoidSlot());
    }
    if (FAILED(Asked))
    {
        // The layer that would read the answers goes with it.
        RtGlassAskPipelines[0].Release();
        RtGlassAskPipelines[1].Release();
        RtGlassMirrorPipelines[0].Release();
        RtGlassMirrorPipelines[1].Release();
        RdxLog(TEXT("glass: the pipeline that writes the mirror rays down for the 64-bit host was refused (0x%08X) - the ")
               TEXT("panes give nothing back"), (UINT)Asked);
        return;
    }
    RdxLog(TEXT("glass: the mirror layer's pipelines for the 64-bit host's rays made - the rays written down, and the ")
           TEXT("layer that reads their answers"));
}

UBOOL FSolidPass::RebuildPipelines(ID3D12Device* Device)
{
    for (INT Set = 0; Set < PIPE_SETS; Set++)
    {
        WaterMarkPipelines[Set].Release();
        RtWaterMarkPipelines[Set].Release();
    }
    RtGlassMirrorPipelines[0].Release();
    RtGlassMirrorPipelines[1].Release();
    RtGlassAskPipelines[0].Release();
    RtGlassAskPipelines[1].Release();
    for (INT Set = 0; Set < PIPE_SETS; Set++)
    {
        RtHostWaterPipelines[Set].Release();
        RtWaterAskPipelines[Set].Release();
        RtWaterAskMarkPipelines[Set].Release();
        RtHostSludgeMirrorPipelines[Set].Release();
        RtSludgeMirrorAskPipelines[Set].Release();
        RtHostSplashPipeline[Set].Release();
        RtSplashAskPipeline[Set].Release();
    }
    RtHostPaneKeepPipeline.Release();
    RtPaneKeepAskPipeline.Release();
    for (INT i = 0; i < BLEND_MAX; i++)
    {
        Pipelines[0][i].Release();
        Pipelines[1][i].Release();
    }
    FlashPipeline[0].Release();
    FlashPipeline[1].Release();
    SplashPipeline[0].Release();
    SplashPipeline[1].Release();
    return CreatePipelineState(Device);
}

UBOOL FSolidPass::CreateVertexBuffers(ID3D12Device* Device)
{
    D3D12_HEAP_PROPERTIES Heap;
    appMemzero(&Heap, sizeof(Heap));
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC Desc;
    appMemzero(&Desc, sizeof(Desc));
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = (UINT64)MAX_VERTICES * sizeof(FSolidVertex);
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (INT i = 0; i < BACKEND_FRAME_COUNT; i++)
    {
        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   __uuidof(ID3D12Resource), (void**)&VertexBuffers[i])))
        {
            RdxLog(TEXT("vertex buffer %i allocation failed"), i);
            return 0;
        }

        D3D12_RANGE NoRead;
        NoRead.Begin = 0;
        NoRead.End   = 0;
        if (FAILED(VertexBuffers[i]->Map(0, &NoRead, (void**)&Mapped[i])))
        {
            RdxLog(TEXT("vertex buffer %i map failed"), i);
            return 0;
        }
    }

    // The flashlight's blocks (SetBeam): a frame slot's four each, and one of zeros after them - every run binds one
    // of them, so there has to be a buffer.
    {
        D3D12_RANGE NoRead;
        NoRead.Begin = 0;
        NoRead.End   = 0;
        Desc.Width = (UINT64)BEAM_BLOCK_BYTES * (BEAM_KINDS * BACKEND_FRAME_COUNT + 1);
        if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   __uuidof(ID3D12Resource), (void**)&BeamBuffer)) ||
            FAILED(BeamBuffer->Map(0, &NoRead, (void**)&BeamMapped)) || !BeamMapped)
        {
            RdxLog(TEXT("the flashlight's constant buffer could not be made"));
            return 0;
        }
        appMemzero(BeamMapped, BEAM_BLOCK_BYTES * (BEAM_KINDS * BACKEND_FRAME_COUNT + 1));
    }

    // The relief's held edges' blocks (SetReliefHeld), a buffer a frame slot. Without them the patches' corners carry the
    // rim band, as they do wherever a surface's edges are too many for a block.
    Desc.Width = HELD_BYTES;
    for (INT i = 0; i < BACKEND_FRAME_COUNT; i++)
    {
        D3D12_RANGE NoRead;
        NoRead.Begin = 0;
        NoRead.End   = 0;
        if (SUCCEEDED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      __uuidof(ID3D12Resource), (void**)&HeldBuffers[i])) &&
            SUCCEEDED(HeldBuffers[i]->Map(0, &NoRead, (void**)&HeldMapped[i])))
            appMemzero(HeldMapped[i], HELD_ZERO);
        else
        {
            RdxLog(TEXT("relief held-edge buffer %i could not be made - the patches carry the rim band"), i);
            if (HeldBuffers[i])
            {
                HeldBuffers[i]->Release();
                HeldBuffers[i] = nullptr;
            }
            HeldMapped[i] = nullptr;
        }
    }

    // The grass's blocks (GrassBlock), a buffer a frame slot. Without one that frame slot draws no grass.
    Desc.Width = GRASS_BYTES + GRASS_TAIL;
    for (INT i = 0; i < BACKEND_FRAME_COUNT; i++)
    {
        D3D12_RANGE NoRead;
        NoRead.Begin = 0;
        NoRead.End   = 0;
        if (SUCCEEDED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      __uuidof(ID3D12Resource), (void**)&GrassBuffers[i])) &&
            SUCCEEDED(GrassBuffers[i]->Map(0, &NoRead, (void**)&GrassMapped[i])))
            appMemzero(GrassMapped[i], GRASS_BYTES + GRASS_TAIL);
        else
        {
            RdxLog(TEXT("grass buffer %i could not be made - no grass in that frame slot"), i);
            if (GrassBuffers[i])
            {
                GrassBuffers[i]->Release();
                GrassBuffers[i] = nullptr;
            }
            GrassMapped[i] = nullptr;
        }
    }

    return 1;
}

UBOOL FSolidPass::Init(ID3D12Device* Device, UBOOL bStencilKinds_)
{
    bStencilKinds = bStencilKinds_ ? 1 : 0;
    CurrentKind   = 0;
    CurrentSprite = 0;
    if (!CreateRootSignature(Device)) return 0;
    if (!CreatePipelineState(Device)) return 0;
    if (!CreateVertexBuffers(Device)) return 0;
    RdxLog(TEXT("geometry pass ready (%i vertices, %i draws max)"), (INT)MAX_VERTICES, (INT)MAX_RUNS);
    return 1;
}

void FSolidPass::Shutdown()
{
    for (INT Set = 0; Set < PIPE_SETS; Set++)
    {
        WaterMarkPipelines[Set].Release();
        RtWaterMarkPipelines[Set].Release();
    }
    RtGlassMirrorPipelines[0].Release();
    RtGlassMirrorPipelines[1].Release();
    RtGlassAskPipelines[0].Release();
    RtGlassAskPipelines[1].Release();
    for (INT Set = 0; Set < PIPE_SETS; Set++)
    {
        RtHostWaterPipelines[Set].Release();
        RtWaterAskPipelines[Set].Release();
        RtWaterAskMarkPipelines[Set].Release();
        RtHostSludgeMirrorPipelines[Set].Release();
        RtSludgeMirrorAskPipelines[Set].Release();
        RtHostSplashPipeline[Set].Release();
        RtSplashAskPipeline[Set].Release();
    }
    RtHostPaneKeepPipeline.Release();
    RtPaneKeepAskPipeline.Release();
    TessPipelines[0].Release();
    TessPipelines[1].Release();
    TessModPipelines[0].Release();
    TessModPipelines[1].Release();
    TessFogPipelines[0].Release();
    TessFogPipelines[1].Release();
    GrassPipelines[0].Release();
    GrassPipelines[1].Release();
    TreePipelines[0].Release();
    TreePipelines[1].Release();
    DustPipelines[0].Release();
    DustPipelines[1].Release();
    CoverPipeline.Release();
    RtWaterRoot.Release();
    FlashPipeline[0].Release();
    FlashPipeline[1].Release();
    SplashPipeline[0].Release();
    SplashPipeline[1].Release();
    for (INT i = 0; i < BACKEND_FRAME_COUNT; i++)
    {
        if (VertexBuffers[i])
        {
            if (Mapped[i])
            {
                VertexBuffers[i]->Unmap(0, nullptr);
                Mapped[i] = nullptr;
            }
            VertexBuffers[i]->Release();
            VertexBuffers[i] = nullptr;
        }
        if (HeldBuffers[i])
        {
            if (HeldMapped[i])
                HeldBuffers[i]->Unmap(0, nullptr);
            HeldBuffers[i]->Release();
            HeldBuffers[i] = nullptr;
        }
        HeldMapped[i] = nullptr;
        if (GrassBuffers[i])
        {
            if (GrassMapped[i])
                GrassBuffers[i]->Unmap(0, nullptr);
            GrassBuffers[i]->Release();
            GrassBuffers[i] = nullptr;
        }
        GrassMapped[i] = nullptr;
    }
    if (BeamBuffer)
    {
        if (BeamMapped)
            BeamBuffer->Unmap(0, nullptr);
        BeamBuffer->Release();
        BeamBuffer = nullptr;
    }
    BeamMapped = nullptr;
    for (INT i = 0; i < BEAM_KINDS; i++)
        BeamOn[i] = 0;

    Dest = nullptr;

    for (INT i = 0; i < BLEND_MAX; i++)
    {
        Pipelines[0][i].Release();
        Pipelines[1][i].Release();
    }
    RootSignature.Release();
}

// ---------------------------------------------------------------------------

void FSolidPass::SetBeam(INT FrameIndex, const FLOAT* Block, const FLOAT* Thin, const FLOAT* Mirrors,
                         INT MirrorCount, const FLOAT* Own, const FLOAT* Views)
{
    for (INT i = 0; i < BEAM_KINDS; i++)
        BeamOn[i] = 0;
    const UBOOL bCone = (Block && Thin) ? 1 : 0;
    if ((!bCone && !Own) || !BeamBuffer || !BeamMapped || FrameIndex < 0 || FrameIndex >= BACKEND_FRAME_COUNT)
        return;
    const FLOAT* const From[2] = { Block, Thin };
    for (INT i = 0; i < BEAM_KINDS; i++)
    {
        // A view the backend gives no place in the level takes no lamp; and a block with neither is the zeros'.
        const FLOAT* const View   = Views ? Views + (i / 2) * BEAM_VIEW_FLOATS : nullptr;
        const UBOOL        bLamps = (Own && View && View[11] > 0.5f) ? 1 : 0;
        if (!bCone && !bLamps)
            continue;
        const INT    At  = (BEAM_KINDS * FrameIndex + i) * BEAM_BLOCK_BYTES;
        FLOAT* const Out = (FLOAT*)(BeamMapped + At);
        if (bCone)
            appMemcpy(Out, From[i & 1], RdxMath::BeamBlockFloats * sizeof(FLOAT));
        else
            appMemzero(Out, RdxMath::BeamBlockFloats * sizeof(FLOAT));
        // The mirrors that give the cone back: in the world's view's two blocks alone.
        const INT N = (i < 2 && Mirrors && bCone) ? Clamp(MirrorCount, 0, (INT)BEAM_MIRRORS) : 0;
        Out[RdxMath::BeamBlockFloats]     = (FLOAT)N;
        Out[RdxMath::BeamBlockFloats + 1] = Out[RdxMath::BeamBlockFloats + 2] = 0.0f;
        Out[RdxMath::BeamBlockFloats + 3] = 0.0f;
        if (N > 0)
            appMemcpy(Out + RdxMath::BeamBlockFloats + 4, Mirrors, (SIZE_T)N * 8 * sizeof(FLOAT));
        // The own lamps: the view's place and its projection, the share its see-through surfaces take, the rows.
        FLOAT* const O = Out + BEAM_OWN_FIRST;
        if (bLamps)
        {
            appMemcpy(O, View, BEAM_VIEW_FLOATS * sizeof(FLOAT));
            O[3] = (i & 1) ? RdxMath::BeamSeeThrough : 1.0f;
            appMemcpy(O + BEAM_VIEW_FLOATS, Own, OWN_FLOATS * sizeof(FLOAT));
        }
        else
            appMemzero(O, BEAM_VIEW_FLOATS * sizeof(FLOAT));
        BeamOn[i] = BeamBuffer->GetGPUVirtualAddress() + (UINT64)At;
    }
}

FLOAT* FSolidPass::GrassBlock(INT Rows, D3D12_GPU_VIRTUAL_ADDRESS& Address)
{
    Address = 0;
    if (Rows <= 0 || Rows > (INT)RdxMath::GrassBlockRows || HeldFrame < 0 || !GrassMapped[HeldFrame] ||
        !GrassBuffers[HeldFrame])
        return nullptr;
    // A root view's address goes by 256 bytes.
    const INT Bytes = (Rows * 16 + 255) & ~255;
    if (GrassUsed + Bytes > (INT)GRASS_BYTES)
    {
        if (!bGrassFullSaid)
        {
            bGrassFullSaid = 1;
            RdxLog(TEXT("grass: a frame's %i KB of blocks are used up - the rest of that frame's grass is not drawn"),
                   (INT)(GRASS_BYTES >> 10));
        }
        return nullptr;
    }
    FLOAT* const Out = (FLOAT*)(GrassMapped[HeldFrame] + GrassUsed);
    Address = GrassBuffers[HeldFrame]->GetGPUVirtualAddress() + (UINT64)GrassUsed;
    GrassUsed += Bytes;
    return Out;
}

void FSolidPass::AddGrassDraw(D3D12_GPU_VIRTUAL_ADDRESS Block, INT Instances, INT SrvIndex, INT LightSrv,
                              UBOOL bNoSmooth, UBOOL bWorldView)
{
    if (!Block || Instances <= 0 || SrvIndex < 0)
        return;
    if (RunCount >= MAX_RUNS)
    {
        if (!bOverflowReported)
        {
            RdxLog(TEXT("draw list full at %i runs - grass dropped"), RunCount);
            bOverflowReported = 1;
        }
        return;
    }
    // A solid run of the ground's, as far as the flush's order goes - it stays where it was put, among the world's
    // solid runs - with no vertices: never extended (EnsureRun), drawn by the grass's own pipeline.
    FRun& R = Runs[RunCount++];
    R.Start            = VertexCount;
    R.Count            = 0;
    R.SrvIndex         = SrvIndex;
    R.LightSrv         = LightSrv;
    R.Mode             = BLEND_Opaque;
    R.bClearDepthFirst = 0;
    R.bNoSmooth        = bNoSmooth ? 1 : 0;
    R.bFlash           = 0;
    R.bTargetSwitch    = 0;
    R.bWorldStart      = 0;
    R.bWorldEnd        = 0;
    R.GrassInstances   = Instances;
    R.Tree             = 0;
    R.bDust            = 0;
    R.GrassBlock       = Block;
    // The grass's own kind (2): the shadows' factor shades a blade as the ground it stands on (ShadowFactorSurface.hlsli).
    R.Kind             = bStencilKinds ? 2 : 0;
    R.bSprite          = 0;
    R.Beam             = CurrentBeamLit;
    R.bPom             = 0;
    R.PomTable         = -1;
    appMemzero(R.Pom, sizeof(R.Pom));
    appMemzero(R.Water, sizeof(R.Water));
    R.bRelief          = 0;
    R.bTess            = 0;
    R.bRepeat          = 0;
    R.bInMirror        = bInMirrorNow;
    R.ReliefBlock      = 0;
    GrassRuns++;
    if (bWorldView && !bInMirrorNow)
        NotePlantMotion(0, Block, nullptr, nullptr, 0, (UINT)RdxMath::GrassBladeVerts, 0, (UINT)Instances, -1);
}

void FSolidPass::AddDustDraw(D3D12_GPU_VIRTUAL_ADDRESS Block, INT Instances, INT SrvIndex)
{
    if (!Block || Instances <= 0 || SrvIndex < 0)
        return;
    if (RunCount >= MAX_RUNS)
    {
        if (!bOverflowReported)
        {
            RdxLog(TEXT("draw list full at %i runs - dust dropped"), RunCount);
            bOverflowReported = 1;
        }
        return;
    }
    // A see-through run as far as the flush's order goes - laid with the world's other see-through things, after its
    // solid ones - with no vertices of the pass's own: never extended (EnsureRun).
    FRun& R = Runs[RunCount++];
    R.Start            = VertexCount;
    R.Count            = 0;
    R.SrvIndex         = SrvIndex;
    R.LightSrv         = -1;
    R.Mode             = BLEND_Translucent;
    R.bClearDepthFirst = 0;
    R.bNoSmooth        = 0;
    R.bFlash           = 0;
    R.bTargetSwitch    = 0;
    R.bWorldStart      = 0;
    R.bWorldEnd        = 0;
    R.GrassInstances   = Instances;
    R.GrassBlock       = Block;
    R.Tree             = 0;
    R.bDust            = 1;
    R.Kind             = 0;
    R.bSprite          = 0;
    R.Beam             = CurrentBeamLit;
    R.bPom             = 0;
    R.PomTable         = -1;
    appMemzero(R.Pom, sizeof(R.Pom));
    appMemzero(R.Water, sizeof(R.Water));
    R.bRelief          = 0;
    R.bTess            = 0;
    R.bRepeat          = 0;
    R.bInMirror        = bInMirrorNow;
    R.ReliefBlock      = 0;
}

void FSolidPass::AddTreeDraw(const D3D12_VERTEX_BUFFER_VIEW& Vb, const D3D12_INDEX_BUFFER_VIEW& Ib, INT IndexStart,
                             INT IndexCount, D3D12_GPU_VIRTUAL_ADDRESS Block, INT SrvIndex, UBOOL bWorldView)
{
    if (!Block || IndexCount <= 0 || SrvIndex < 0 || !Vb.BufferLocation || !Ib.BufferLocation)
        return;
    if (RunCount >= MAX_RUNS || TreeDrawCount >= (INT)TREE_DRAWS)
    {
        if (!bOverflowReported)
        {
            RdxLog(TEXT("draw list full at %i runs, %i plants' draws - a plant dropped"), RunCount, TreeDrawCount);
            bOverflowReported = 1;
        }
        return;
    }
    FTreeDraw& D    = TreeDrawList[TreeDrawCount++];
    D.Vb            = Vb;
    D.Ib            = Ib;
    D.IndexStart    = (UINT)IndexStart;
    D.IndexCount    = (UINT)IndexCount;
    D.InstanceStart = 0;
    D.InstanceCount = 1;
    D.bLeaves       = 0;
    D.Block         = Block;
    // A solid run as far as the flush's order goes, with no vertices of the pass's own: never extended (EnsureRun).
    FRun& R = Runs[RunCount++];
    R.Start            = VertexCount;
    R.Count            = 0;
    R.SrvIndex         = SrvIndex;
    R.LightSrv         = -1;
    R.Mode             = BLEND_Opaque;
    R.bClearDepthFirst = 0;
    R.bNoSmooth        = 0;
    R.bFlash           = 0;
    R.bTargetSwitch    = 0;
    R.bWorldStart      = 0;
    R.bWorldEnd        = 0;
    R.GrassInstances   = 0;
    R.GrassBlock       = 0;
    R.Tree             = TreeDrawCount;
    R.bDust            = 0;
    R.Kind             = bStencilKinds ? CurrentKind : 0;
    R.bSprite          = 0;
    R.Beam             = CurrentBeamLit;
    R.bPom             = 0;
    R.PomTable         = -1;
    appMemzero(R.Pom, sizeof(R.Pom));
    appMemzero(R.Water, sizeof(R.Water));
    R.bRelief          = 0;
    R.bTess            = 0;
    R.bRepeat          = 0;
    R.bInMirror        = bInMirrorNow;
    R.ReliefBlock      = 0;
    if (bWorldView && !bInMirrorNow)
        NotePlantMotion(1, Block, &Vb, &Ib, (UINT)IndexStart, (UINT)IndexCount, 0, 1, SrvIndex);
}

void FSolidPass::AddLeafDraw(const D3D12_VERTEX_BUFFER_VIEW& Vb, const D3D12_INDEX_BUFFER_VIEW& Ib, INT FrameFirst,
                             INT FrameIndices, INT MotionFirst, INT MotionIndices, INT First, INT Count,
                             D3D12_GPU_VIRTUAL_ADDRESS Block, INT SrvIndex, UBOOL bWorldView)
{
    // A plant's run of the bark's shape, then made the leaves' own.
    const INT Before = TreeDrawCount;
    AddTreeDraw(Vb, Ib, FrameFirst, FrameIndices, Block, SrvIndex, 0);
    if (TreeDrawCount == Before || Count <= 0)
        return;
    FTreeDraw& D    = TreeDrawList[TreeDrawCount - 1];
    D.InstanceStart = (UINT)First;
    D.InstanceCount = (UINT)Count;
    D.bLeaves       = 1;
    if (bWorldView && !bInMirrorNow)
        NotePlantMotion(2, Block, &Vb, &Ib, (UINT)MotionFirst, (UINT)MotionIndices, (UINT)First, (UINT)Count, SrvIndex);
}

void FSolidPass::NotePlantMotion(INT Kind, D3D12_GPU_VIRTUAL_ADDRESS Block, const D3D12_VERTEX_BUFFER_VIEW* Vb,
                                 const D3D12_INDEX_BUFFER_VIEW* Ib, UINT Start, UINT Count, UINT InstanceStart,
                                 UINT InstanceCount, INT Srv)
{
    if (PlantMotionUsed >= PLANT_MOTIONS)
        return;
    FPlantMotion& M = PlantMotions[PlantMotionUsed++];
    appMemzero(&M, sizeof(M));
    M.Kind  = Kind;
    M.Block = Block;
    if (Vb)
        M.Vb = *Vb;
    if (Ib)
        M.Ib = *Ib;
    M.Start         = Start;
    M.Count         = Count;
    M.InstanceStart = InstanceStart;
    M.InstanceCount = InstanceCount;
    M.Srv           = Srv;
}

// The plants' motion vectors' pipelines, for the scene's depth as it is (its format with the shadows and without): a
// root of the two cameras' constants (b3), a plant's block (b2) and a leaf's picture (t0, s0); depth-tested as the
// bodies' are (FFinalPass::InitMotionObjects), never written, the vector into the motion target.
UBOOL FSolidPass::EnsurePlantMotion(ID3D12Device* Device, DXGI_FORMAT DsvFormat)
{
    if (PlantMotionState != 0 && PlantMotionFormat == DsvFormat)
        return PlantMotionState > 0;
    PlantMotionFormat = DsvFormat;
    PlantMotionState  = -1;
    for (INT k = 0; k < 3; k++)
        PlantMotionPso[k].Release();
    for (INT k = 0; k < 3; k++)
        if (!PlantMotionVs[k])
            return 0;
    if (!PlantMotionPs[0] || !PlantMotionPs[1])
        return 0;
    if (!PlantMotionRoot)
    {
        D3D12_DESCRIPTOR_RANGE Range;
        appMemzero(&Range, sizeof(Range));
        Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Range.NumDescriptors     = 1;
        Range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER Params[3];
        appMemzero(Params, sizeof(Params));
        Params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        Params[0].Constants.ShaderRegister            = 3;
        Params[0].Constants.Num32BitValues            = 48;
        Params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[1].Descriptor.ShaderRegister           = 2;
        Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;
        Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[2].DescriptorTable.NumDescriptorRanges = 1;
        Params[2].DescriptorTable.pDescriptorRanges   = &Range;
        Params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC Sampler;
        appMemzero(&Sampler, sizeof(Sampler));
        Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC Root;
        appMemzero(&Root, sizeof(Root));
        Root.NumParameters     = 3;
        Root.pParameters       = Params;
        Root.NumStaticSamplers = 1;
        Root.pStaticSamplers   = &Sampler;
        Root.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        TComRef<ID3DBlob> Blob, Error;
        if (FAILED(D3D12SerializeRootSignature(&Root, D3D_ROOT_SIGNATURE_VERSION_1, Blob.Slot(), Error.Slot())) ||
            FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                               __uuidof(ID3D12RootSignature), PlantMotionRoot.VoidSlot())))
        {
            RdxLog(TEXT("the plants' motion vectors: the root signature failed - their camera's vector alone"));
            return 0;
        }
    }
    // The bark's vertices (TreeGen.h's FVertex) and a leaf's instance (FLeafInstance), as the geometry pass reads them.
    D3D12_INPUT_ELEMENT_DESC Bark[6], Leaf[8];
    appMemzero(Bark, sizeof(Bark));
    appMemzero(Leaf, sizeof(Leaf));
    const char* const BarkNames[6]   = { "POSITION", "NORMAL", "TEXCOORD", "TEXCOORD", "TEXCOORD", "COLOR" };
    const UINT        BarkIndices[6] = { 0, 0, 0, 1, 2, 0 };
    const DXGI_FORMAT BarkFormats[6] = { DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
                                         DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                         DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM };
    const UINT        BarkOffsets[6] = { 0, 12, 24, 32, 40, 48 };
    for (INT k = 0; k < 6; k++)
    {
        Bark[k].SemanticName      = BarkNames[k];
        Bark[k].SemanticIndex     = BarkIndices[k];
        Bark[k].Format            = BarkFormats[k];
        Bark[k].AlignedByteOffset = BarkOffsets[k];
    }
    const char* const LeafNames[8]   = { "POSITION", "TEXCOORD", "NORMAL", "TANGENT", "COLOR", "TEXCOORD", "TEXCOORD",
                                         "TEXCOORD" };
    const UINT        LeafIndices[8] = { 0, 0, 0, 0, 0, 1, 2, 3 };
    const DXGI_FORMAT LeafFormats[8] = { DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                         DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM,
                                         DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                         DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
    const UINT        LeafOffsets[8] = { 0, 12, 16, 24, 32, 36, 40, 48 };
    for (INT k = 0; k < 8; k++)
    {
        Leaf[k].SemanticName         = LeafNames[k];
        Leaf[k].SemanticIndex        = LeafIndices[k];
        Leaf[k].Format               = LeafFormats[k];
        Leaf[k].AlignedByteOffset    = LeafOffsets[k];
        Leaf[k].InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        Leaf[k].InstanceDataStepRate = 1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Pso;
    appMemzero(&Pso, sizeof(Pso));
    Pso.pRootSignature                   = PlantMotionRoot.Get();
    Pso.PrimitiveTopologyType            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Pso.NumRenderTargets                 = 1;
    Pso.RTVFormats[0]                    = DXGI_FORMAT_R16G16_FLOAT;
    Pso.DSVFormat                        = DsvFormat;
    Pso.SampleDesc.Count                 = 1;
    Pso.SampleMask                       = UINT_MAX;
    Pso.RasterizerState.FillMode         = D3D12_FILL_MODE_SOLID;
    Pso.RasterizerState.CullMode         = D3D12_CULL_MODE_NONE;
    Pso.RasterizerState.DepthClipEnable  = TRUE;
    Pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    Pso.DepthStencilState.DepthEnable    = TRUE;
    Pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    Pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    for (INT k = 0; k < 3; k++)
    {
        ID3DBlob* const Ps = PlantMotionPs[(k == 2) ? 1 : 0].Get();
        Pso.VS.pShaderBytecode             = PlantMotionVs[k]->GetBufferPointer();
        Pso.VS.BytecodeLength              = PlantMotionVs[k]->GetBufferSize();
        Pso.PS.pShaderBytecode             = Ps->GetBufferPointer();
        Pso.PS.BytecodeLength              = Ps->GetBufferSize();
        // As many elements as the vertex shaders read: the arrays' own counts, so that a layout cannot fall short of the
        // vertex it describes - a pipeline whose shader reads an element the layout leaves out is refused.
        Pso.InputLayout.pInputElementDescs = (k == 0) ? nullptr : ((k == 1) ? Bark : Leaf);
        Pso.InputLayout.NumElements        = (k == 0) ? 0 : ((k == 1) ? (UINT)ARRAY_COUNT(Bark) : (UINT)ARRAY_COUNT(Leaf));
        const HRESULT Made = Device->CreateGraphicsPipelineState(&Pso, __uuidof(ID3D12PipelineState),
                                                                 PlantMotionPso[k].VoidSlot());
        if (FAILED(Made))
        {
            RdxLog(TEXT("the plants' motion vectors: pipeline %i failed (0x%08X) - their camera's vector alone"), k,
                   (UINT)Made);
            for (INT j = 0; j < 3; j++)
                PlantMotionPso[j].Release();
            return 0;
        }
    }
    PlantMotionState = 1;
    RdxLog(TEXT("the plants' motion vectors: the pipelines up"));
    return 1;
}

void FSolidPass::DrawPlantMotion(ID3D12Device* Device, ID3D12GraphicsCommandList* Cmd, FTextureCache& Textures,
                                 DXGI_FORMAT DsvFormat, const FLOAT* Constants, INT Kind)
{
    if (PlantMotionUsed <= 0 || !Constants || Kind < 0 || Kind > 2 || !EnsurePlantMotion(Device, DsvFormat))
        return;
    INT    Count = 0;
    UINT64 Laid  = 0;
    for (INT i = 0; i < PlantMotionUsed; i++)
        if (PlantMotions[i].Kind == Kind)
        {
            Count++;
            Laid += (UINT64)PlantMotions[i].Count * (UINT64)PlantMotions[i].InstanceCount;
        }
    // What each kind lays, told every 600 frames of the leaves' turn: its draws and its corners (indices times
    // instances) a frame.
    PlantMotionDraws[Kind] += Count;
    PlantMotionLaid[Kind] += Laid;
    if (Kind == 2 && ++PlantMotionFrames >= 600)
    {
        RdxLog(TEXT("the plants' motion vectors over %i frames, a frame: the grass %.1f draws, %.0f corners; the bark %.1f "
                    "draws, %.0f corners; the leaves %.1f draws, %.0f corners"), PlantMotionFrames,
               (FLOAT)PlantMotionDraws[0] / PlantMotionFrames, (DOUBLE)PlantMotionLaid[0] / PlantMotionFrames,
               (FLOAT)PlantMotionDraws[1] / PlantMotionFrames, (DOUBLE)PlantMotionLaid[1] / PlantMotionFrames,
               (FLOAT)PlantMotionDraws[2] / PlantMotionFrames, (DOUBLE)PlantMotionLaid[2] / PlantMotionFrames);
        PlantMotionFrames = 0;
        for (INT k = 0; k < 3; k++)
        {
            PlantMotionDraws[k] = 0;
            PlantMotionLaid[k]  = 0;
        }
    }
    if (Count == 0)
        return;
    ID3D12DescriptorHeap* Heaps[1] = { Textures.GetHeap() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(PlantMotionRoot.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, 48, Constants, 0);
    Cmd->SetPipelineState(PlantMotionPso[Kind].Get());
    Cmd->IASetPrimitiveTopology((Kind == 0) ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (INT i = 0; i < PlantMotionUsed; i++)
    {
        const FPlantMotion& M = PlantMotions[i];
        if (M.Kind != Kind)
            continue;
        Cmd->SetGraphicsRootConstantBufferView(1, M.Block);
        if (M.Kind == 0)
        {
            Cmd->DrawInstanced(M.Count, M.InstanceCount, 0, 0);
            continue;
        }
        if (M.Kind == 2)
            Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(M.Srv));
        Cmd->IASetVertexBuffers(0, 1, &M.Vb);
        Cmd->IASetIndexBuffer(&M.Ib);
        Cmd->DrawIndexedInstanced(M.Count, M.InstanceCount, M.Start, 0, M.InstanceStart);
    }
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void FSolidPass::Begin(INT InWidth, INT InHeight, INT FrameIndex)
{
    PlantMotionUsed = 0;
    Dest = (FrameIndex >= 0 && FrameIndex < BACKEND_FRAME_COUNT) ? Mapped[FrameIndex] : nullptr;
    VertexCount = 0;
    RunCount    = 0;
    HeldFrame      = (FrameIndex >= 0 && FrameIndex < BACKEND_FRAME_COUNT) ? FrameIndex : -1;
    HeldUsed       = HELD_ZERO;
    ReliefBlockNow = 0;
    GrassUsed      = 0;
    GrassRuns      = 0;
    TreeDrawCount  = 0;
    Width  = InWidth  > 0 ? InWidth: 1;
    Height = InHeight > 0 ? InHeight : 1;
}

// Extend the current run when nothing about it has changed; otherwise open a new one.
// Consecutive geometry sharing a texture is the common case, which is exactly why the engine's
// draw order is worth preserving rather than sorting.
UBOOL FSolidPass::EnsureRun(INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth)
{
    // The kind splits runs only where the stencil carries it; without stencil kinds it takes no part in the batching -
    // but for the see-through and the modulated draws, where the relief's covers tell the level's sheets from the
    // actors' sprites, decals and shadows by it (DrawReliefCovers). Those are few.
    const INT Kind = (bStencilKinds || Mode == BLEND_Translucent || Mode == BLEND_Modulated) ? CurrentKind : 0;
    if (RunCount > 0 &&
        Runs[RunCount - 1].Kind == Kind &&
        Runs[RunCount - 1].bSprite == CurrentSprite &&
        Runs[RunCount - 1].Beam == CurrentBeamLit &&
        !Runs[RunCount - 1].bClearDepthFirst &&
        !Runs[RunCount - 1].bFlash &&
        !Runs[RunCount - 1].bTargetSwitch &&
        !Runs[RunCount - 1].bWorldStart &&
        !Runs[RunCount - 1].bWorldEnd &&
        !Runs[RunCount - 1].bRepeat &&
        !Runs[RunCount - 1].GrassInstances &&
        !Runs[RunCount - 1].Tree &&
        Runs[RunCount - 1].bInMirror == bInMirrorNow &&
        Runs[RunCount - 1].SrvIndex  == SrvIndex &&
        Runs[RunCount - 1].LightSrv  == LightSrv &&
        Runs[RunCount - 1].Mode      == Mode &&
        Runs[RunCount - 1].bNoSmooth == bNoSmooth &&
        Runs[RunCount - 1].PomTable  == PomTableNow &&
        appMemcmp(Runs[RunCount - 1].Pom, PomNow, sizeof(PomNow)) == 0 &&
        Runs[RunCount - 1].bRelief == bReliefNow &&
        Runs[RunCount - 1].bTess == bTessNow &&
        Runs[RunCount - 1].ReliefBlock == ReliefBlockNow &&
        ((Mode != BLEND_Water && Mode != BLEND_Fall && Mode != BLEND_Sludge && Mode != BLEND_SludgeFog &&
          Mode != BLEND_SludgeMirror && Mode != BLEND_GlassMirror && Mode != BLEND_Body) ||
         appMemcmp(Runs[RunCount - 1].Water, WaterNow, sizeof(WaterNow)) == 0) &&
        (!bReliefNow || appMemcmp(Runs[RunCount - 1].Water, ReliefNow, sizeof(ReliefNow)) == 0))
        return 1;

    if (RunCount >= MAX_RUNS)
    {
        // Never drop geometry silently: a dropped run leaves a hole in the picture and nothing else to point at it.
        if (!bOverflowReported)
        {
            RdxLog(TEXT("draw list full at %i runs - geometry dropped"), RunCount);
            bOverflowReported = 1;
        }
        return 0;
    }
    Runs[RunCount].Start    = VertexCount;
    Runs[RunCount].Count    = 0;
    Runs[RunCount].bPom     = (PomTableNow >= 0) ? 1 : 0;
    Runs[RunCount].PomTable = PomTableNow;
    appMemcpy(Runs[RunCount].Pom, PomNow, sizeof(PomNow));
    Runs[RunCount].SrvIndex = SrvIndex;
    Runs[RunCount].LightSrv = LightSrv;
    Runs[RunCount].Mode     = Mode;
    Runs[RunCount].bNoSmooth = bNoSmooth;
    Runs[RunCount].bFlash    = 0;
    Runs[RunCount].bTargetSwitch = 0;
    Runs[RunCount].bWorldStart   = 0;
    Runs[RunCount].bWorldEnd     = 0;
    Runs[RunCount].GrassInstances = 0;
    Runs[RunCount].Tree           = 0;
    Runs[RunCount].bDust          = 0;
    Runs[RunCount].bClearDepthFirst = 0;
    Runs[RunCount].Kind      = Kind;
    Runs[RunCount].bSprite   = CurrentSprite;
    Runs[RunCount].Beam      = CurrentBeamLit;
    Runs[RunCount].bRelief   = bReliefNow;
    Runs[RunCount].bTess     = bTessNow;
    Runs[RunCount].bRepeat   = 0;
    Runs[RunCount].bInMirror = bInMirrorNow;
    Runs[RunCount].ReliefBlock = ReliefBlockNow;
    appMemcpy(Runs[RunCount].Water, bReliefNow ? ReliefNow : WaterNow, sizeof(WaterNow));
    RunCount++;
    return 1;
}

D3D12_GPU_VIRTUAL_ADDRESS FSolidPass::SetReliefHeld(const FLOAT* Lamps, INT LampFloats, const FLOAT* Ends, INT Count)
{
    ReliefBlockNow = 0;
    if (Count <= 0 || Count > RELIEF_HELD_MAX || HeldFrame < 0 || !HeldMapped[HeldFrame] || !Ends)
        return 0;
    // The shader's layout (WaterLampConsts): the lamps' count and two rows a lamp, then how many held edges, then three
    // rows an edge. A constant buffer's address is a multiple of 256.
    enum { LampRows = 1 + 2 * RdxMath::ReliefLampMax };
    const INT Bytes = ((LampRows + 1 + 3 * Count) * 16 + 255) & ~255;
    if (HeldUsed + Bytes > HELD_BYTES)
    {
        if (!bHeldFullSaid)
        {
            RdxLog(TEXT("relief held edges: the frame's %i bytes are full - the rest carry the rim band on their corners"),
                   (INT)HELD_BYTES);
            bHeldFullSaid = 1;
        }
        return 0;
    }
    // Written in order and never read back: the upload heap is write-combined.
    FLOAT* const Out = (FLOAT*)(HeldMapped[HeldFrame] + HeldUsed);
    for (INT i = 0; i < LampRows * 4; i++)
        Out[i] = (Lamps && i < LampFloats) ? Lamps[i] : 0.0f;
    FLOAT* At = Out + LampRows * 4;
    At[0] = (FLOAT)Count;
    At[1] = At[2] = At[3] = 0.0f;
    At += 4;
    for (INT e = 0; e < Count * 12; e++)
        At[e] = Ends[e];
    ReliefBlockNow = HeldBuffers[HeldFrame]->GetGPUVirtualAddress() + (UINT64)HeldUsed;
    HeldUsed += Bytes;
    return ReliefBlockNow;
}

UBOOL FSolidPass::AddRepeatRun(INT Start, INT Count, INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth)
{
    if (!Dest || Count <= 0 || Start < 0 || Start + Count > VertexCount)
        return 0;
    if (RunCount >= MAX_RUNS)
    {
        if (!bOverflowReported)
        {
            RdxLog(TEXT("draw list full at %i runs - geometry dropped"), RunCount);
            bOverflowReported = 1;
        }
        return 0;
    }
    FRun& R = Runs[RunCount++];
    R.Start            = Start;
    R.Count            = Count;
    R.SrvIndex         = SrvIndex;
    R.LightSrv         = LightSrv;
    R.Mode             = Mode;
    R.bNoSmooth        = bNoSmooth;
    R.bClearDepthFirst = 0;
    R.bFlash           = 0;
    R.bTargetSwitch    = 0;
    R.bWorldStart      = 0;
    R.bWorldEnd        = 0;
    R.GrassInstances   = 0;
    R.Tree             = 0;
    R.bDust            = 0;
    R.Kind             = bStencilKinds ? CurrentKind : 0;
    R.bSprite          = CurrentSprite;
    R.Beam             = CurrentBeamLit;
    R.bPom             = (PomTableNow >= 0) ? 1 : 0;
    R.PomTable         = PomTableNow;
    appMemcpy(R.Pom, PomNow, sizeof(PomNow));
    R.bRelief          = bReliefNow;
    R.bTess            = bTessNow;
    R.bRepeat          = 1;
    R.ReliefBlock      = ReliefBlockNow;
    appMemcpy(R.Water, bReliefNow ? ReliefNow : WaterNow, sizeof(WaterNow));
    return 1;
}

FSolidVertex* FSolidPass::BeginTriangle(INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth)
{
    return BeginTriangles(1, SrvIndex, LightSrv, Mode, bNoSmooth);
}

FSolidVertex* FSolidPass::BeginTriangles(INT Count, INT SrvIndex, INT LightSrv, EBlendMode Mode, UBOOL bNoSmooth)
{
    if (!Dest || Count <= 0)
        return nullptr;
    if (VertexCount + 3 * Count > MAX_VERTICES)
    {
        if (!bOverflowReported)
        {
            RdxLog(TEXT("vertex buffer full at %i - geometry dropped"), VertexCount);
            bOverflowReported = 1;
        }
        return nullptr;
    }
    if (!EnsureRun(SrvIndex, LightSrv, Mode, bNoSmooth))
        return nullptr;

    FSolidVertex* const V = Dest + VertexCount;
    VertexCount += 3 * Count;
    Runs[RunCount - 1].Count += 3 * Count;
    return V;
}

void FSolidPass::AddFan(const FSolidVertex* Verts, INT Count, INT SrvIndex, INT LightSrv, EBlendMode Mode,
                        UBOOL bNoSmooth)
{
    if (!Dest || Count < 3)
        return;

    const INT Needed = (Count - 2) * 3;
    if (VertexCount + Needed > MAX_VERTICES)
    {
        if (!bOverflowReported)
        {
            RdxLog(TEXT("vertex buffer full at %i - geometry dropped"), VertexCount);
            bOverflowReported = 1;
        }
        return;
    }

    if (!EnsureRun(SrvIndex, LightSrv, Mode, bNoSmooth))
        return;

    for (INT i = 1; i + 1 < Count; i++)
    {
        Dest[VertexCount++] = Verts[0];
        Dest[VertexCount++] = Verts[i];
        Dest[VertexCount++] = Verts[i + 1];
    }
    Runs[RunCount - 1].Count += Needed;
}

void FSolidPass::AddDepthClear()
{
    if (RunCount >= MAX_RUNS)
        return;

    // An empty run carrying only the marker. Kept separate from the next run so the
    // clear cannot be lost when that run merges with the one before it.
    Runs[RunCount].Start    = VertexCount;
    Runs[RunCount].Count    = 0;
    Runs[RunCount].SrvIndex = -1;
    Runs[RunCount].LightSrv = -1;
    Runs[RunCount].Mode     = BLEND_Opaque;
    Runs[RunCount].bNoSmooth = 0;
    Runs[RunCount].bFlash    = 0;
    Runs[RunCount].bTargetSwitch = 0;
    Runs[RunCount].bWorldStart   = 0;
    Runs[RunCount].bWorldEnd     = 0;
    Runs[RunCount].GrassInstances = 0;
    Runs[RunCount].Tree           = 0;
    Runs[RunCount].bDust          = 0;
    Runs[RunCount].bClearDepthFirst = 1;
    // A marker run draws nothing, so its kind is never used - but it is read in EnsureRun's merge test, and a field
    // left over from the previous frame has no business being read at all.
    Runs[RunCount].Kind      = bStencilKinds ? CurrentKind : 0;
    Runs[RunCount].bSprite   = 0;
    Runs[RunCount].Beam      = 0;
    RunCount++;
}

void FSolidPass::AddMirrorEnd(INT Index)
{
    if (Index < 0 || RunCount >= MAX_RUNS)
        return;
    // The clear's own marker, told from it by its sign: below zero no clear, only the owner's pass - the view's
    // number and two more, taken from nought. A marker to everything that asks whether a run is one.
    AddDepthClear();
    Runs[RunCount - 1].bClearDepthFirst = -(2 + Index);
}

void FSolidPass::AddFlash(FLOAT ScaleR, FLOAT ScaleG, FLOAT ScaleB, FLOAT FogR, FLOAT FogG, FLOAT FogB)
{
    if (RunCount >= MAX_RUNS)
        return;

    // A marker, like the depth clear: it has to take effect where the engine asked for it -
    // after what was drawn so far, before what comes next - not wherever the batch happens
    // to be submitted.
    FRun& R = Runs[RunCount++];
    R.Start            = VertexCount;
    R.Count            = 0;
    R.SrvIndex         = -1;
    R.LightSrv         = -1;
    R.Mode             = BLEND_Opaque;
    R.bClearDepthFirst = 0;
    R.bNoSmooth        = 0;
    R.bFlash           = 1;
    R.bTargetSwitch    = 0;
    R.bWorldStart      = 0;
    R.bWorldEnd        = 0;
    R.GrassInstances   = 0;
    R.Tree             = 0;
    R.bDust            = 0;
    R.Kind             = bStencilKinds ? CurrentKind : 0;
    R.bSprite          = 0;
    R.Beam             = 0;
    R.Flash[0] = ScaleR; R.Flash[1] = ScaleG; R.Flash[2] = ScaleB;
    R.Flash[3] = FogR;   R.Flash[4] = FogG;   R.Flash[5] = FogB;
}

UBOOL FSolidPass::AddTargetSwitch()
{
    if (RunCount >= MAX_RUNS)
        return 0;

    // A marker like the depth clear: what was recorded before it goes to the scene target, what comes
    // after it to the output-sized one.
    FRun& R = Runs[RunCount++];
    R.Start            = VertexCount;
    R.Count            = 0;
    R.SrvIndex         = -1;
    R.LightSrv         = -1;
    R.Mode             = BLEND_Opaque;
    R.bClearDepthFirst = 0;
    R.bNoSmooth        = 0;
    R.bFlash           = 0;
    R.bTargetSwitch    = 1;
    R.bWorldStart      = 0;
    R.bWorldEnd        = 0;
    R.GrassInstances   = 0;
    R.Tree             = 0;
    R.bDust            = 0;
    R.Kind             = bStencilKinds ? CurrentKind : 0;
    R.bSprite          = 0;
    R.Beam             = 0;
    return 1;
}

UBOOL FSolidPass::AddWorldStart()
{
    if (RunCount >= MAX_RUNS)
        return 0;
    // An empty run carrying only the marker, like the depth clear's.
    FRun& R = Runs[RunCount++];
    R.Start            = VertexCount;
    R.Count            = 0;
    R.SrvIndex         = -1;
    R.LightSrv         = -1;
    R.Mode             = BLEND_Opaque;
    R.bClearDepthFirst = 0;
    R.bNoSmooth        = 0;
    R.bFlash           = 0;
    R.bTargetSwitch    = 0;
    R.bWorldStart      = 1;
    R.bWorldEnd        = 0;
    R.GrassInstances   = 0;
    R.Tree             = 0;
    R.bDust            = 0;
    R.Kind             = bStencilKinds ? CurrentKind : 0;
    R.bSprite          = 0;
    R.Beam             = 0;
    return 1;
}

UBOOL FSolidPass::AddWorldEnd()
{
    if (RunCount >= MAX_RUNS)
        return 0;
    // An empty run carrying only the marker, like the world start's.
    FRun& R = Runs[RunCount++];
    R.Start            = VertexCount;
    R.Count            = 0;
    R.SrvIndex         = -1;
    R.LightSrv         = -1;
    R.Mode             = BLEND_Opaque;
    R.bClearDepthFirst = 0;
    R.bNoSmooth        = 0;
    R.bFlash           = 0;
    R.bTargetSwitch    = 0;
    R.bWorldStart      = 0;
    R.bWorldEnd        = 1;
    R.GrassInstances   = 0;
    R.Tree             = 0;
    R.bDust            = 0;
    R.Kind             = bStencilKinds ? CurrentKind : 0;
    R.bSprite          = 0;
    R.Beam             = 0;
    return 1;
}

// SetModeStats' sums: [blend mode][surface kind 0 BSP, 1 actor], and the frames they cover.
static INT    GModeRuns[BLEND_MAX][2];
static INT    GModeTris[BLEND_MAX][2];
static DOUBLE GModeArea[BLEND_MAX][2];
static INT    GModeFrames = 0;

// The covers are bound (FTargetSwitch::BeginReliefCovers) and hold the depth drawn so far - every run before From. The
// world's runs from From on that can stand in front of a raised surface go into them too, flat, depth alone: what writes
// depth - the level's and the actors' opaque and masked surfaces, the flat surfaces under the relief's patches, the
// see-through surfaces that claim the depth - and the level's own see-through surfaces: a mirror, a pane of glass, and
// the modulated ones - a film of grime over a window, a screen set in front of a wall (222 of those lie within a raised
// wall's relief, and the relief came through them). Not the patches themselves, nor what is laid over a surface -
// the decals and shadows, which are the actors' modulated draws, a see-through sheet of the level lying on a raised
// surface, which is brought forward over the relief instead and comes as one of those (RenderDevice.cpp,
// ReliefDecalLift), and the detail textures' second runs - nor the actors' see-through sprites - smoke, flares - nor
// the water. The world ends at its end marker, at a depth clear or at the switch to the output.
INT FSolidPass::DrawReliefCovers(ID3D12GraphicsCommandList* Cmd, FTextureCache& Textures, INT From, INT NumOrder,
                                 INT& Modulated)
{
    Cmd->SetPipelineState(CoverPipeline.Get());
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    INT Srv = -1, NoSmooth = -1, Drawn = 0;
    Modulated = 0;
    for (INT At = From; At < NumOrder; At++)
    {
        const FRun& R = Runs[RunOrder[At]];
        if (R.bWorldEnd || R.bClearDepthFirst || R.bTargetSwitch)
            break;
        if (R.bFlash || R.bWorldStart || R.Count <= 0 || R.SrvIndex < 0 || R.bTess || R.bRepeat)
            continue;
        const UBOOL bLevelModulated = (R.Mode == BLEND_Modulated && R.Kind == 0 && bCoverModulated) ? 1 : 0;
        if (!(R.Mode == BLEND_Opaque || R.Mode == BLEND_TranslucentOccluding ||
              (R.Mode == BLEND_Translucent && R.Kind == 0) || bLevelModulated))
            continue;
        Modulated += bLevelModulated;
        if (R.SrvIndex != Srv)
        {
            Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(R.SrvIndex));
            Srv = R.SrvIndex;
        }
        if ((INT)R.bNoSmooth != NoSmooth)
        {
            NoSmooth = R.bNoSmooth ? 1 : 0;
            Cmd->SetGraphicsRoot32BitConstant(3, (UINT)NoSmooth, 0);
        }
        Cmd->DrawInstanced(R.Count, 1, R.Start, 0);
        Drawn++;
    }
    return Drawn;
}

void FSolidPass::Flush(ID3D12GraphicsCommandList* Cmd, INT FrameIndex, FTextureCache& Textures,
                       D3D12_CPU_DESCRIPTOR_HANDLE Dsv, FTargetSwitch* Switch)
{
    // The frame's vertices are already in this slot's buffer (see Dest); from here on, until the
    // next Begin, nothing may write there.
    FSolidVertex* const Filled = Dest;
    Dest = nullptr;
    if (!VertexCount || !Pipelines[0][BLEND_Opaque] || !Mapped[FrameIndex])
        return;
    if (Filled != Mapped[FrameIndex])
    {
        static UBOOL bReported = 0;
        if (!bReported)
        {
            bReported = 1;
            RdxLog(TEXT("vertices: we wrote into a different frame slot from the one being drawn (%i) - skipped"),
                   FrameIndex);
        }
        return;
    }

    D3D12_VERTEX_BUFFER_VIEW View;
    View.BufferLocation = VertexBuffers[FrameIndex]->GetGPUVirtualAddress();
    View.SizeInBytes    = VertexCount * sizeof(FSolidVertex);
    View.StrideInBytes  = sizeof(FSolidVertex);

    // The jitter belongs to the WORLD and to nothing else: it starts with the world's first geometry (bWorldStart),
    // stops at the switch to the output target where the HUD begins, and never runs in a frame without a world, like
    // the main menu. Every draw goes through this one vertex shader, so a jitter left switched on shifts the HUD and
    // the menu too - and a temporal upscaler resolves the world's offsets from the motion vectors, which the HUD has
    // none of. There its shake would simply be a shake.
    FLOAT Constants[6] = { 2.0f / (FLOAT)Width, 2.0f / (FLOAT)Height, 0.0f, 0.0f, LightBoost, 0.0f };

    ID3D12DescriptorHeap* Heaps[1] = { Textures.GetHeap() };
    Cmd->SetDescriptorHeaps(1, Heaps);
    Cmd->SetGraphicsRootSignature(RootSignature.Get());
    Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd->IASetVertexBuffers(0, 1, &View);

    INT CurrentMode = -1;
    INT CurrentNoSmooth = -1;
    INT CurrentStencil  = -1;
    // The two texture tables, remembered. A run is opened whenever ANY of kind, texture, light map, blend mode or
    // filtering changes, so consecutive runs very often share both textures and differ only in the mode - and setting
    // a descriptor table that is already set is pure cost. Reset wherever the root signature is set again below.
    INT CurrentSrv      = -1;
    // Whether the four spare constants may hold anything but zeros - and after a root signature is set they hold
    // whatever they hold, so 1 there as here: the next run writes its own four either way. Taken as zeros, a flat run
    // drawn first would read a height-map switch never written, and look for a height map never bound.
    INT CurrentPom      = 1;
    INT CurrentLightSrv = -1;
    // Which set of pipelines is in force. The world is multisampled; from the switch on, the HUD is not - and the
    // pipelines have to follow the targets exactly, sample for sample.
    INT PipeSet = (bMsaaLive && MsaaSamples > 1 && Pipelines[1][BLEND_Opaque]) ? 1 : 0;
    // The water's copies: made at the frame's first water run, once (FTargetSwitch::CopyForWater).
    UBOOL bWaterTried  = 0;
    UBOOL bWaterCopied = 0;
    // What the copy of the depth misses: where in the frame it was made, and how many runs that write the depth - the
    // level's and the actors' - still came after it before the world ended. The smoke, the waterfalls and the soft
    // particles stop at that copy, so a thing drawn after it is not there for them (reported below, sparsely).
    INT   CopyAtOrder = -1, CopyMode = -1;
    UBOOL bCopySoft = 0, bCopyCounting = 0;
    INT   DepthAfterCopy[2] = { 0, 0 };
    // What the copy of the picture misses: the runs laid on the picture since the copies were last made - a water
    // surface among them or not, and the others counted; a fall's sheet after another fall's is not one. A run that
    // makes its whole pixel from the copy wipes those out where it stands in front of them: a fall's sheet gets the
    // copies made again (CopiesAgain: how often this frame), and what a water surface missed is reported below.
    UBOOL bWaterAfterCopy = 0;
    INT   OthersAfterCopy = 0, CopiesAgain = 0, WaterMissed = 0;
    // The relief's covers: made before a view's first patches, once a view (FTargetSwitch::BeginReliefCovers) - a
    // mirror's view has patches and covers of its own, and the world's view after it is not to read those.
    UBOOL bCoverTried = 0;
    UBOOL bCoverMade  = 0;
    // Reflections=RT (FTargetSwitch::BeginMirrorAsk): whether its runs draw with rays this frame, what they bind, and
    // whether its root signature is the one set - it is for the RT water's runs alone.
    UBOOL        bRtWater   = 0;
    UBOOL        bRtRootSet = 0;
    FRtWaterBind RtBind;
    appMemzero(&RtBind, sizeof(RtBind));
    UBOOL bTopologyPatches = 0;       // the topology last set: 1 after a water run, until the next other run
    // The water's splashes (FTargetSwitch::DrawSplashes): the ones the camera is under before the first water surface,
    // the rest where the world ends - each laid on with one triangle over the whole target, with this pass's own state
    // set again around it, as the splashes' passes draw with a root signature of their own.
    UBOOL bWorldSeen   = 0;
    UBOOL bSplashTried = 0;
    // Mirror: -1 for the world's view; for a mirror's view of the level the owner's number for it - that view's
    // splashes, laid on without the rays: the rays' scene is the world's view's.
    auto LaySplashes = [&](UBOOL bUnderWater, INT Mirror)
    {
        FLOAT SplashConsts[WATER_CONSTANTS];
        if (!Switch || !SplashPipeline[PipeSet] || !WaterLamps || SplashSmoothView < 0 ||
            !((Mirror >= 0) ? Switch->DrawMirrorSplashes(Cmd, Dsv, Constants, Mirror, SplashConsts)
                            : Switch->DrawSplashes(Cmd, Dsv, Constants, SplashConsts, bUnderWater)))
            return;
        // On rays (the 64-bit host's, SetRtHosted) the water's own mirror shows the room in the falling water. The
        // splashes' rays are a round of their own: the one triangle drawn with the build that writes its pixels'
        // rays down, the host asked - the frame is split there - and the triangle then drawn below with the build
        // that reads the answers.
        UBOOL bRays = 0;
        if (Mirror < 0 && bRtHosted && RtSplashPipe(PipeSet) && RtSplashAskPipeline[PipeSet] && WaterTable >= 0 &&
            Switch->BeginMirrorAsk(Cmd, RtBind, Dsv))
        {
            Cmd->SetDescriptorHeaps(1, Heaps);
            Cmd->SetGraphicsRootSignature(RtWaterRoot.Get());
            Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
            Cmd->SetGraphicsRootShaderResourceView(6, RtBind.Scene);
            Cmd->SetGraphicsRootShaderResourceView(7, RtBind.Table);
            Cmd->SetGraphicsRootShaderResourceView(8, RtBind.FrameUvw);
            Cmd->SetGraphicsRootShaderResourceView(9, RtBind.ArenaUvw);
            Cmd->SetGraphicsRootDescriptorTable(10, RtBind.Masks);
            Cmd->SetGraphicsRootShaderResourceView(11, RtBind.LevelMaterial);
            Cmd->SetGraphicsRootDescriptorTable(12, Textures.GetGpuHandle(0));
            Cmd->SetGraphicsRootDescriptorTable(ROOT_WATER_UAV_RT, RtBind.Asked);
            Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(SplashSmoothView));
            Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(SplashThickView));
            Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(WaterTable));
            Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, SplashConsts, 4);
            Cmd->SetGraphicsRootConstantBufferView(5, WaterLamps);
            Cmd->SetPipelineState(RtSplashAskPipeline[PipeSet].Get());
            Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            Cmd->DrawInstanced(3, 1, 0, 0);
            static INT Told = 0;
            if ((Told++ % 600) == 0)
                RdxLog(TEXT("rays' list: the splashes asked, a round of their own (under water: %i)"),
                       (INT)bUnderWater);
            bRays = Switch->EndMirrorAsk(Cmd, Dsv, RtBind);
        }
        Cmd->SetDescriptorHeaps(1, Heaps);
        Cmd->SetGraphicsRootSignature(bRays ? RtWaterRoot.Get() : RootSignature.Get());
        bRtRootSet = 0;
        Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
        if (bRays)
        {
            Cmd->SetGraphicsRootShaderResourceView(6, RtBind.Scene);
            Cmd->SetGraphicsRootShaderResourceView(7, RtBind.Table);
            Cmd->SetGraphicsRootShaderResourceView(8, RtBind.FrameUvw);
            Cmd->SetGraphicsRootShaderResourceView(9, RtBind.ArenaUvw);
            Cmd->SetGraphicsRootDescriptorTable(10, RtBind.Masks);
            Cmd->SetGraphicsRootShaderResourceView(11, RtBind.LevelMaterial);
            Cmd->SetGraphicsRootDescriptorTable(12, Textures.GetGpuHandle(0));
        }
        Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(SplashSmoothView));
        Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(SplashThickView));
        Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(WaterTable));
        Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, SplashConsts, 4);
        Cmd->SetGraphicsRootConstantBufferView(5, WaterLamps);
        Cmd->SetPipelineState(bRays ? RtSplashPipe(PipeSet) : SplashPipeline[PipeSet].Get());
        Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bTopologyPatches = 0;
        Cmd->DrawInstanced(3, 1, 0, 0);
        Cmd->IASetVertexBuffers(0, 1, &View);
        CurrentMode     = -1;
        CurrentNoSmooth = -1;
        CurrentPom      = 1;
        CurrentStencil  = -1;
        CurrentSrv      = -1;
        CurrentLightSrv = -1;
    };
    // Reflections=RT: the RT water's root signature - the pass's own parameters, and the rays' after them - for a run
    // on rays, set once until something else needs the pass's own back.
    auto UseRtRoot = [&]()
    {
        if (bRtRootSet)
            return;
        Cmd->SetGraphicsRootSignature(RtWaterRoot.Get());
        Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
        Cmd->SetGraphicsRootShaderResourceView(6, RtBind.Scene);
        Cmd->SetGraphicsRootShaderResourceView(7, RtBind.Table);
        Cmd->SetGraphicsRootShaderResourceView(8, RtBind.FrameUvw);
        Cmd->SetGraphicsRootShaderResourceView(9, RtBind.ArenaUvw);
        Cmd->SetGraphicsRootDescriptorTable(10, RtBind.Masks);
        Cmd->SetGraphicsRootShaderResourceView(11, RtBind.LevelMaterial);
        Cmd->SetGraphicsRootDescriptorTable(12, Textures.GetGpuHandle(0));
        bRtRootSet      = 1;
        CurrentMode     = -1;
        CurrentNoSmooth = -1;
        CurrentPom      = 1;
        CurrentSrv      = -1;
        CurrentLightSrv = -1;
    };
    // The panes of glass into the mirrors' map, before anything of the world is drawn (PSPaneKeep): the flashlight's
    // light off them is looked up there by the world's surfaces. Every pane's mirror layer of the frame that carries a
    // number (its constant 2) - the layer's own runs, drawn here with the rays alone.
    if (Switch && RtPaneKeepPipe() && RtWaterRoot)
    {
        INT First = -1;
        for (INT i = 0; i < RunCount && First < 0; i++)
            if (Runs[i].Mode == BLEND_GlassMirror && Runs[i].Water[31] <= 0.5f && Runs[i].Water[2] > 0.5f &&
                Runs[i].Count > 0 && Runs[i].SrvIndex >= 0)
                First = i;
        FRtWaterBind PaneBind;
        // The map's size, and how far from the view's axis it is ever looked up, as a cosine (0: not known).
        FLOAT        PaneSize[3] = { 1.0f, 1.0f, 0.0f };
        // The panes drawn with a pipeline of the pass on the rays' root signature, everything bound for it; bAsk:
        // with the rays' list bound to be written.
        const auto DrawPanes = [&](ID3D12PipelineState* Pso, UBOOL bAsk)
        {
            Cmd->SetDescriptorHeaps(1, Heaps);
            Cmd->SetGraphicsRootSignature(RtWaterRoot.Get());
            Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
            Cmd->SetGraphicsRootShaderResourceView(6, PaneBind.Scene);
            Cmd->SetGraphicsRootShaderResourceView(7, PaneBind.Table);
            Cmd->SetGraphicsRootShaderResourceView(8, PaneBind.FrameUvw);
            Cmd->SetGraphicsRootShaderResourceView(9, PaneBind.ArenaUvw);
            Cmd->SetGraphicsRootDescriptorTable(10, PaneBind.Masks);
            Cmd->SetGraphicsRootShaderResourceView(11, PaneBind.LevelMaterial);
            Cmd->SetGraphicsRootDescriptorTable(12, Textures.GetGpuHandle(0));
            if (bAsk)
                Cmd->SetGraphicsRootDescriptorTable(ROOT_WATER_UAV_RT, PaneBind.Asked);
            Cmd->SetPipelineState(Pso);
            Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            Cmd->IASetVertexBuffers(0, 1, &View);
            for (INT i = First; i < RunCount; i++)
            {
                const FRun& R = Runs[i];
                if (R.Mode != BLEND_GlassMirror || R.Water[31] > 0.5f || R.Water[2] <= 0.5f || R.Count <= 0 ||
                    R.SrvIndex < 0)
                    continue;
                FLOAT Pane[WATER_CONSTANTS];
                for (INT k = 0; k < WATER_CONSTANTS; k++)
                    Pane[k] = R.Water[k];
                Pane[0] = PaneSize[0];
                Pane[1] = PaneSize[1];
                Pane[3] = PaneSize[2];
                Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(R.SrvIndex));
                Cmd->SetGraphicsRoot32BitConstant(3, R.bNoSmooth ? 1u : 0u, 0);
                Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, Pane, 4);
                Cmd->DrawInstanced(R.Count, 1, R.Start, 0);
            }
        };
        // The panes' rays are asked first, a round of their own: the panes drawn with the build that writes their
        // rays down, the host asked - the frame is split there - and then into the map with the build that reads
        // the answers. Not answered: no panes in the map this frame.
        UBOOL bRays = 0;
        if (First >= 0 && Switch->BeginPaneAsk(Cmd, PaneBind, PaneSize))
        {
            DrawPanes(RtPaneKeepAskPipeline.Get(), 1);
            static INT Told = 0;
            if ((Told++ % 600) == 0)
                RdxLog(TEXT("rays' list: the panes asked, a round of their own before the world"));
            bRays = Switch->EndMirrorAsk(Cmd, Dsv, PaneBind);
        }
        if (First >= 0 && bRays && Switch->BeginPaneKeep(Cmd, PaneBind, PaneSize))
        {
            DrawPanes(RtPaneKeepPipe(), 0);
            Switch->EndPaneKeep(Cmd, Dsv);
        }
        if (First >= 0)
        {
            // The pass's own again, as it stood before the panes - whatever came of them: a round to the host
            // leaves nothing set on the list.
            Cmd->SetDescriptorHeaps(1, Heaps);
            Cmd->SetGraphicsRootSignature(RootSignature.Get());
            Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
            Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            Cmd->IASetVertexBuffers(0, 1, &View);
            bRtRootSet      = 0;
            CurrentMode     = -1;
            CurrentNoSmooth = -1;
            CurrentPom      = 1;
            CurrentSrv      = -1;
            CurrentLightSrv = -1;
        }
    }
    // The order the runs go in: as they came, with two exceptions inside the world.
    //
    // The blended runs - translucent, modulated, and the tiles that leave the depth alone (the world's sprites) - wait,
    // and go after the runs that write depth, among themselves in the order they came. The engine sorts its actors by
    // one point each, so a see-through actor and a solid one close behind it can come the wrong way round: the solid one
    // drawn after the see-through one in front of it covers it, as nothing of the see-through one is in the depth (the
    // ERROR save: the water cooler over the holographic screen, looking a few degrees down). Laid on after everything
    // that writes depth, a blended surface is laid over exactly what stands behind it. They wait until the next water
    // surface (its copy of the picture is taken at the first one, and has to hold what is behind it and nothing in front),
    // the next see-through surface that claims the depth (a mirror floor, which is laid over its reflection), the next
    // marker, or the end - and those keep their place.
    //
    // The world's tessellated runs wait too, and go together - before the blended ones, before any run that is not plain
    // opaque and has not waited, before any marker, and at the end. Drawn where they came, among the flat surfaces, every
    // raised surface switches the pipeline to tessellation and back again, and in a wide view the switching costs more
    // than the patches. Plain opaque runs can go in any order, the depth test sorts them; the flat surface under the
    // patches lies a hair behind them, and the patches win over it whichever comes first.
    INT NumOrder = 0, NumWaiting = 0, NumLater = 0, NumInMirror = 0;
    {
        UBOOL bInWorld = 0;
        auto PutWaiting = [&]()
        {
            for (INT k = 0; k < NumWaiting; k++)
                RunOrder[NumOrder++] = RunWaiting[k];
            NumWaiting = 0;
        };
        // A mirror's whole image (a BLEND_GlassMirror run that gives all of it back - its constant 31) goes first
        // among them: it stands for what lies behind the mirror's face, and every other blended thing before the
        // mirror is laid over it, as it was over the engine's own image.
        // After the mirrors' images, and before the world's own blended runs: what the engine drew through the
        // traced mirrors (SetInMirror) - once the world is being drawn; it came before the world began, and waits.
        auto PutLater = [&]()
        {
            for (INT Whole = 1; Whole >= 0; Whole--)
            {
                for (INT k = 0; k < NumLater; k++)
                {
                    const FRun& L = Runs[RunLater[k]];
                    if (((L.Mode == BLEND_GlassMirror && L.Water[31] > 0.5f) ? 1 : 0) == Whole)
                        RunOrder[NumOrder++] = RunLater[k];
                }
                if (Whole == 1 && bInWorld)
                {
                    for (INT k = 0; k < NumInMirror; k++)
                        RunOrder[NumOrder++] = RunInMirror[k];
                    NumInMirror = 0;
                }
            }
            NumLater = 0;
        };
        for (INT r = 0; r < RunCount; r++)
        {
            const FRun& R       = Runs[r];
            const UBOOL bMarker = (R.bWorldStart || R.bWorldEnd || R.bClearDepthFirst || R.bTargetSwitch || R.bFlash)
                                  ? 1 : 0;
            const UBOOL bBlended = (R.Mode == BLEND_Translucent || R.Mode == BLEND_Modulated ||
                                    R.Mode == BLEND_OpaqueNoZWrite || R.Mode == BLEND_Fog ||
                                    R.Mode == BLEND_FogEqual || R.Mode == BLEND_Fall || R.Mode == BLEND_SludgeFog ||
                                    R.Mode == BLEND_SludgeMirror || R.Mode == BLEND_GlassMirror ||
                                    R.Mode == BLEND_Body) ? 1 : 0;
            if (R.bInMirror && !bMarker)
            {
                RunInMirror[NumInMirror++] = r;
                continue;
            }
            if (bBlendedLast && bInWorld && !bMarker && bBlended)
            {
                RunLater[NumLater++] = r;
                continue;
            }
            if (bTessTogether && bInWorld && !bMarker && R.bTess && !R.bRepeat && R.Mode == BLEND_Opaque)
            {
                RunWaiting[NumWaiting++] = r;
                continue;
            }
            if (bMarker || R.Mode != BLEND_Opaque || R.bRepeat)
                PutWaiting();
            if (bMarker || R.Mode == BLEND_Water || R.Mode == BLEND_TranslucentOccluding)
                PutLater();
            if (R.bWorldStart)
                bInWorld = 1;
            if (R.bWorldEnd || R.bClearDepthFirst || R.bTargetSwitch)
                bInWorld = 0;
            RunOrder[NumOrder++] = r;
        }
        PutWaiting();
        PutLater();
    }
    // The rays through the 64-bit host (SetRtHosted; docs/terv-rt-host.md), for the runs on rays from the one at
    // From to the one before To in the flush's order - a stretch that one view's depth serves. Each is drawn once
    // with the build that only writes down the rays its pixels would trace, into the rays' list - no target, the
    // depth tested as the run's own drawing tests it; a see-through water run after its nearest points are marked,
    // as it is drawn. The host then traces them: the frame is split there, and nothing set on the list survives
    // it - the owner binds the scene's targets again, and the caller sets again what this pass had set. The runs
    // are then drawn in their places with the builds that read the answers. False: no rays for the stretch.
    INT  AskedTo = 0;           // the flush's order up to here has had its rays asked
    auto AskRays = [&](INT From, INT To) -> UBOOL
    {
        const D3D12_GPU_VIRTUAL_ADDRESS Zeros = (HeldFrame >= 0 && HeldBuffers[HeldFrame])
                                                ? HeldBuffers[HeldFrame]->GetGPUVirtualAddress() : 0;
        // What a run asks with - the pipeline that writes its rays down, the lamps' block its shader reads, and
        // whether its triangles go in as patches - or nothing, where the run is not drawn on rays: the same
        // conditions its drawing goes by.
        const auto AskWith = [&](const FRun& R, D3D12_GPU_VIRTUAL_ADDRESS& Lamps, UBOOL& bPatches)
        {
            ID3D12PipelineState* Ask = nullptr;
            Lamps    = 0;
            bPatches = 0;
            if (R.Count <= 0 || R.SrvIndex < 0)
                return Ask;
            if (R.Mode == BLEND_GlassMirror && Pipelines[PipeSet][BLEND_GlassMirror] && RtGlassMirrorPipe(PipeSet) &&
                Zeros)
            {
                Ask   = RtGlassAskPipelines[PipeSet].Get();
                Lamps = Zeros;
            }
            else if (R.Mode == BLEND_Water && WaterLamps && RtWaterPipe(PipeSet))
            {
                Ask      = RtWaterAskPipelines[PipeSet].Get();
                Lamps    = WaterLamps;
                bPatches = 1;
            }
            else if (R.Mode == BLEND_SludgeMirror && WaterLamps && CanSludge() &&
                     Pipelines[PipeSet][BLEND_SludgeMirror] && RtSludgeMirrorPipe(PipeSet))
            {
                Ask      = RtSludgeMirrorAskPipelines[PipeSet].Get();
                Lamps    = WaterLamps;
                bPatches = 1;
            }
            return Ask;
        };
        UBOOL bAny = 0;
        for (INT At = From; At < To && !bAny; At++)
        {
            D3D12_GPU_VIRTUAL_ADDRESS Lamps;
            UBOOL bPatches;
            bAny = AskWith(Runs[RunOrder[At]], Lamps, bPatches) ? 1 : 0;
        }
        if (!bAny || !Switch->BeginMirrorAsk(Cmd, RtBind, Dsv))
            return 0;
        Cmd->SetDescriptorHeaps(1, Heaps);
        Cmd->SetGraphicsRootSignature(RtWaterRoot.Get());
        Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
        Cmd->SetGraphicsRootShaderResourceView(6, RtBind.Scene);
        Cmd->SetGraphicsRootShaderResourceView(7, RtBind.Table);
        Cmd->SetGraphicsRootShaderResourceView(8, RtBind.FrameUvw);
        Cmd->SetGraphicsRootShaderResourceView(9, RtBind.ArenaUvw);
        Cmd->SetGraphicsRootDescriptorTable(10, RtBind.Masks);
        Cmd->SetGraphicsRootShaderResourceView(11, RtBind.LevelMaterial);
        Cmd->SetGraphicsRootDescriptorTable(12, Textures.GetGpuHandle(0));
        Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(WaterTable));
        Cmd->SetGraphicsRootDescriptorTable(ROOT_WATER_UAV_RT, RtBind.Asked);
        Cmd->IASetVertexBuffers(0, 1, &View);
        INT Topology = -1;
        // What the stretch asked, for the log: the runs and their vertices by kind - water, glass, sludge.
        INT AskedRuns[3] = { 0, 0, 0 }, AskedVertices[3] = { 0, 0, 0 }, Marked = 0;
        for (INT At = From; At < To; At++)
        {
            const FRun& R = Runs[RunOrder[At]];
            D3D12_GPU_VIRTUAL_ADDRESS Lamps;
            UBOOL bPatches;
            ID3D12PipelineState* const Ask = AskWith(R, Lamps, bPatches);
            if (!Ask)
                continue;
            const INT Kind = (R.Mode == BLEND_Water) ? 0 : (R.Mode == BLEND_GlassMirror) ? 1 : 2;
            AskedRuns[Kind]++;
            AskedVertices[Kind] += R.Count;
            FLOAT Run[WATER_CONSTANTS];
            for (INT k = 0; k < WATER_CONSTANTS; k++)
                Run[k] = R.Water[k];
            // A see-through water run as its drawing has it: the water's own depth there to read, and the run
            // gone over first to mark its nearest points - the asking then keeps those alone, as the drawing will.
            ID3D12PipelineState* Mark = nullptr;
            if (R.Mode == BLEND_Water)
            {
                UBOOL     bSurface    = 0;
                const INT SurfaceView = Switch->WaterSurfaceView(Cmd, bSurface);
                if (bSurface && SurfaceView >= 0 && !bWaterMarkOff)
                    Mark = RtWaterAskMarkPipelines[PipeSet].Get();
                Run[WATER_SURFACE_CONSTANT] = (bSurface && SurfaceView >= 0) ? (Mark ? 2.0f : 1.0f) : 0.0f;
            }
            else if (R.Mode == BLEND_SludgeMirror)
                Run[WATER_SURFACE_CONSTANT] = 1.0f;
            if ((INT)bPatches != Topology)
            {
                Cmd->IASetPrimitiveTopology(bPatches ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                     : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                Topology = (INT)bPatches;
            }
            Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(R.SrvIndex));
            Cmd->SetGraphicsRootDescriptorTable(
                2, Textures.GetGpuHandle(R.LightSrv >= 0 ? R.LightSrv : Textures.GetWhiteIndex()));
            Cmd->SetGraphicsRoot32BitConstant(3, R.bNoSmooth ? 1u : 0u, 0);
            Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, Run, 4);
            Cmd->SetGraphicsRootConstantBufferView(5, Lamps);
            if (Mark)
            {
                Cmd->SetPipelineState(Mark);
                Cmd->DrawInstanced(R.Count, 1, R.Start, 0);
                Switch->WaterSurfaceSettle(Cmd);
                Marked++;
            }
            Cmd->SetPipelineState(Ask);
            Cmd->DrawInstanced(R.Count, 1, R.Start, 0);
        }
        static INT Told = 0;
        if ((Told++ % 601) == 0)
            RdxLog(TEXT("rays' list: a stretch of the flush asked, runs %i to %i of %i - water %i (%i vertices, %i ")
                   TEXT("marked first), glass %i (%i vertices), sludge %i (%i vertices)"), From, To, NumOrder,
                   AskedRuns[0], AskedVertices[0], Marked, AskedRuns[1], AskedVertices[1], AskedRuns[2],
                   AskedVertices[2]);
        return Switch->EndMirrorAsk(Cmd, Dsv, RtBind);
    };
    for (INT OrderAt = 0; OrderAt < NumOrder; OrderAt++)
    {
        const INT i = RunOrder[OrderAt];
        if (Runs[i].bWorldEnd || Runs[i].bClearDepthFirst || Runs[i].bTargetSwitch)
            bCopyCounting = 0;          // the world is over: what comes now is not behind anything in it
        // The world ends at the first depth clear, target switch or world-end marker after it began - the first-person
        // weapon's clear, or the HUD's start: the depth still holds the world there, and the picture has every water
        // surface in it.
        if (bWorldSeen && !bSplashTried &&
            ((INT)Runs[i].bClearDepthFirst > 0 || Runs[i].bTargetSwitch || Runs[i].bWorldEnd))
        {
            bSplashTried = 1;
            LaySplashes(0, -1);
        }
        // And the copy of the depth, if no water and no soft particle made it on the way: here, while the depth still holds
        // the world. Any later - at the resolve - the first-person weapon has cleared it, or moved to a depth of its own, and
        // the copy holds the weapon alone: the smoke and the waterfalls then walk through every wall and board in front of
        // them (measured on the SMOREERROR save: a board across a door, the smoke from behind it laid over it). CopyForWater
        // refuses, touching nothing, when nobody wants the copy; and it may draw and bind targets of its own, so everything
        // this pass had set is set again.
        if (bWorldSeen && !bWaterTried && Switch &&
            (Runs[i].bClearDepthFirst || Runs[i].bTargetSwitch || Runs[i].bWorldEnd))
        {
            bWaterTried  = 1;
            bWaterCopied = Switch->CopyForWater(Cmd, Dsv);
            if (bWaterCopied)
            {
                CopyAtOrder = OrderAt;
                CopyMode    = -1;
            }
            Cmd->SetDescriptorHeaps(1, Heaps);
            Cmd->SetGraphicsRootSignature(RootSignature.Get());
            bRtRootSet = 0;
            Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
            Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            bTopologyPatches = 0;
            Cmd->IASetVertexBuffers(0, 1, &View);
            CurrentMode     = -1;
            CurrentNoSmooth = -1;
            CurrentPom      = 1;
            CurrentStencil  = -1;
            CurrentSrv      = -1;
            CurrentLightSrv = -1;
        }

        // The RT water's root signature is for the runs on rays alone - the water's, and the sludge mirror layer's where it
        // has its pipeline: anything else - a marker, the tint, another blend - gets the pass's own back first, everything
        // bound again.
        const UBOOL bRtKind = ((Runs[i].Mode == BLEND_Water && RtWaterPipe(PipeSet)) ||
                               (Runs[i].Mode == BLEND_SludgeMirror && RtSludgeMirrorPipe(PipeSet)) ||
                               (Runs[i].Mode == BLEND_GlassMirror && RtGlassMirrorPipe(PipeSet))) ? 1 : 0;
        if (bRtRootSet && (!bRtKind || Runs[i].bFlash || Runs[i].bTargetSwitch || Runs[i].bWorldStart ||
                           Runs[i].bWorldEnd))
        {
            Cmd->SetGraphicsRootSignature(RootSignature.Get());
            Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
            bRtRootSet      = 0;
            CurrentMode     = -1;
            CurrentNoSmooth = -1;
            CurrentPom      = 1;
            CurrentSrv      = -1;
            CurrentLightSrv = -1;
        }

        if (Runs[i].bWorldEnd)
            continue;

        if (Runs[i].bTargetSwitch)
        {
            if (Switch)
            {
                Dsv = Switch->SwitchToOutput(Cmd);
                // The HUD follows, at the output's own size: no jitter, and no mip bias.
                Constants[2] = 0.0f;
                Constants[3] = 0.0f;
                Constants[5] = 0.0f;
                // The switch drew with a pipeline of its own: everything this pass had set, set again. With the host's
                // upscaler the switch even hands a list back that has nothing set on it at all (FHostUpscale::Handover) -
                // the stencil reference included.
                Cmd->SetDescriptorHeaps(1, Heaps);
                Cmd->SetGraphicsRootSignature(RootSignature.Get());
                bRtRootSet = 0;
                Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
                Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                bTopologyPatches = 0;
                Cmd->IASetVertexBuffers(0, 1, &View);
                CurrentMode     = -1;
                CurrentNoSmooth = -1;
                CurrentPom      = 1;
                CurrentSrv      = -1;
                CurrentLightSrv = -1;
                CurrentStencil  = -1;
                PipeSet         = 0;      // the HUD draws into the single-sample targets
            }
            continue;
        }

        if (Runs[i].bFlash)
        {
            if (FlashPipeline[PipeSet])
            {
                Cmd->SetPipelineState(FlashPipeline[PipeSet].Get());
                const FLOAT Factor[4] = { Runs[i].Flash[0], Runs[i].Flash[1], Runs[i].Flash[2], 1.0f };
                Cmd->OMSetBlendFactor(Factor);
                // The first value is the NoSmooth switch; 0.0f has the bit pattern of 0.
                const FLOAT Consts[4] = { 0.0f, Runs[i].Flash[3], Runs[i].Flash[4], Runs[i].Flash[5] };
                Cmd->SetGraphicsRoot32BitConstants(3, 4, Consts, 0);
                // A triangle, whatever the run before left set: a water or relief run leaves the patch list, and a
                // triangle pipeline drawn with it is an invalid draw.
                if (bTopologyPatches)
                {
                    Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    bTopologyPatches = 0;
                }
                Cmd->DrawInstanced(3, 1, 0, 0);
                // The next ordinary run sets its own pipeline and switch again.
                CurrentMode     = -1;
                CurrentNoSmooth = -1;
                CurrentPom      = 1;
            }
            continue;
        }

        if (Runs[i].bWorldStart)
        {
            bWorldSeen = 1;
            if (Switch)
                Switch->MarkWorldDepth();
            // From here the world is drawn: the jitter goes on, and the textures' mip bias with it.
            Constants[2] = JitterX;
            Constants[3] = JitterY;
            Constants[5] = MipBias;
            Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
            continue;
        }

        // Grass3D: a ground's blades (AddGrassDraw), right after the ground, with its texture, light map, filtering,
        // stencil kind and flashlight's share, and the grass's own block in b2 - the lamps' register, which a run
        // without relief does not read, and which every run with relief binds for itself. The run's constants are
        // zeros but for the filtering: no relief, no fade.
        if (Runs[i].GrassInstances > 0)
        {
            ID3D12PipelineState* const Pso = Runs[i].bDust ? DustPipelines[PipeSet].Get() : GrassPipelines[PipeSet].Get();
            if (!Pso)
                continue;
            Cmd->SetPipelineState(Pso);
            CurrentMode = -1;
            // A blade is a strip of its own (each instance's is); the triangles' list is set back after it.
            Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            bTopologyPatches = 0;
            UINT RunConsts[4 + WATER_CONSTANTS];
            appMemzero(RunConsts, sizeof(RunConsts));
            RunConsts[0] = Runs[i].bNoSmooth ? 1u : 0u;
            Cmd->SetGraphicsRoot32BitConstants(3, 4 + WATER_CONSTANTS, RunConsts, 0);
            CurrentNoSmooth = (INT)RunConsts[0];
            CurrentPom      = 0;
            if (Runs[i].SrvIndex != CurrentSrv)
            {
                Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(Runs[i].SrvIndex));
                CurrentSrv = Runs[i].SrvIndex;
            }
            const INT GroundLight = Runs[i].LightSrv >= 0 ? Runs[i].LightSrv : Textures.GetWhiteIndex();
            if (GroundLight != CurrentLightSrv)
            {
                Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(GroundLight));
                CurrentLightSrv = GroundLight;
            }
            if (bStencilKinds && Runs[i].Kind != CurrentStencil)
            {
                Cmd->OMSetStencilRef((UINT)Runs[i].Kind);
                CurrentStencil = Runs[i].Kind;
            }
            const INT How = Runs[i].Beam;
            Cmd->SetGraphicsRootConstantBufferView(ROOT_BEAM, (How > 0 && BeamOn[How - 1]) ? BeamOn[How - 1] : BeamOff());
            if (BounceView >= 0)
                Cmd->SetGraphicsRootDescriptorTable(ROOT_BOUNCE, Textures.GetGpuHandle(BounceView));
            Cmd->SetGraphicsRootConstantBufferView(5, Runs[i].GrassBlock);
            Cmd->DrawInstanced(Runs[i].bDust ? 4u : (UINT)RdxMath::GrassBladeVerts, (UINT)Runs[i].GrassInstances, 0, 0);
            Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            continue;
        }

        // Trees3D: a plant's bark or leaves (AddTreeDraw), where the game's plant was drawn: its own vertices and
        // indices, its picture, the neutral light map (its light rides in the block), the stencil kind and flashlight's
        // share of an actor's mesh, and its block in b2. The pass's own vertices are bound again after it.
        if (Runs[i].Tree > 0 && Runs[i].Tree <= TreeDrawCount)
        {
            const FTreeDraw& D = TreeDrawList[Runs[i].Tree - 1];
            ID3D12PipelineState* const Pso = D.bLeaves ? LeafPipelines[PipeSet].Get() : TreePipelines[PipeSet].Get();
            if (!Pso)
                continue;
            Cmd->SetPipelineState(Pso);
            CurrentMode = -1;
            if (bTopologyPatches)
            {
                Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                bTopologyPatches = 0;
            }
            UINT RunConsts[4 + WATER_CONSTANTS];
            appMemzero(RunConsts, sizeof(RunConsts));
            Cmd->SetGraphicsRoot32BitConstants(3, 4 + WATER_CONSTANTS, RunConsts, 0);
            CurrentNoSmooth = 0;
            CurrentPom      = 0;
            if (Runs[i].SrvIndex != CurrentSrv)
            {
                Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(Runs[i].SrvIndex));
                CurrentSrv = Runs[i].SrvIndex;
            }
            const INT White = Textures.GetWhiteIndex();
            if (White != CurrentLightSrv)
            {
                Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(White));
                CurrentLightSrv = White;
            }
            if (bStencilKinds && Runs[i].Kind != CurrentStencil)
            {
                Cmd->OMSetStencilRef((UINT)Runs[i].Kind);
                CurrentStencil = Runs[i].Kind;
            }
            const INT How = Runs[i].Beam;
            Cmd->SetGraphicsRootConstantBufferView(ROOT_BEAM, (How > 0 && BeamOn[How - 1]) ? BeamOn[How - 1] : BeamOff());
            if (BounceView >= 0)
                Cmd->SetGraphicsRootDescriptorTable(ROOT_BOUNCE, Textures.GetGpuHandle(BounceView));
            Cmd->SetGraphicsRootConstantBufferView(5, D.Block);
            Cmd->IASetVertexBuffers(0, 1, &D.Vb);
            Cmd->IASetIndexBuffer(&D.Ib);
            Cmd->DrawIndexedInstanced(D.IndexCount, D.InstanceCount, D.IndexStart, 0, D.InstanceStart);
            Cmd->IASetVertexBuffers(0, 1, &View);
            continue;
        }

        if ((INT)Runs[i].bClearDepthFirst < 0)
        {
            // A mirror's view has drawn its surfaces (AddMirrorEnd): that view's splashes and fountains' drops, then
            // what the owner lays over the whole of it, while the view's depth is still there. The owner draws with a
            // root signature, heaps and a pipeline of its own, so everything this pass had set is set again, as
            // after the clear below.
            if (Switch)
            {
                LaySplashes(0, -(INT)Runs[i].bClearDepthFirst - 2);
                // A measurement (FBackend::MirrorViewDone asked for in the world's own view): the world's
                // splashes have gone on this way, and do not go on again where the world ends.
                if (bWorldSeen)
                    bSplashTried = 1;
                Switch->MirrorViewEnd(Cmd, Dsv, -(INT)Runs[i].bClearDepthFirst - 2);
                Cmd->SetDescriptorHeaps(1, Heaps);
                Cmd->SetGraphicsRootSignature(RootSignature.Get());
                bRtRootSet = 0;
                Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
                Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                bTopologyPatches = 0;
                Cmd->IASetVertexBuffers(0, 1, &View);
                CurrentMode     = -1;
                CurrentNoSmooth = -1;
                CurrentPom      = 1;
                CurrentStencil  = -1;
                CurrentSrv      = -1;
                CurrentLightSrv = -1;
            }
            continue;
        }
        if (Runs[i].bClearDepthFirst)
        {
            // A view ends at the clear: the covers made for its patches are not the next view's.
            bCoverTried = 0;
            bCoverMade  = 0;
            // The owner may keep the depth drawn so far (FTargetSwitch::ClearDepthMidFrame); without one, the clear.
            if (Switch)
            {
                Dsv = Switch->ClearDepthMidFrame(Cmd, Dsv);
                // IT MAY HAVE DRAWN. With MSAA this is where the world's depth is resolved, and a resolve is a whole
                // pass with its own root signature, heaps, pipeline and topology - so everything this pass had set is
                // gone, exactly as after the target switch below, and the next draw would otherwise use the resolve's
                // state. The first-person weapon is that next draw.
                //
                // The jitter stays: this is still the world, not the HUD - so the constants go back as they are.
                Cmd->SetDescriptorHeaps(1, Heaps);
                Cmd->SetGraphicsRootSignature(RootSignature.Get());
                bRtRootSet = 0;
                Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
                Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                bTopologyPatches = 0;
                Cmd->IASetVertexBuffers(0, 1, &View);
                CurrentMode     = -1;
                CurrentNoSmooth = -1;
                CurrentPom      = 1;
                CurrentStencil  = -1;
                CurrentSrv      = -1;
                CurrentLightSrv = -1;
            }
            else
                Cmd->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }

        if (Runs[i].Count <= 0 || Runs[i].SrvIndex < 0)
            continue;

        INT   Mode     = (INT)Runs[i].Mode;
        UBOOL bRtRun   = 0;
        UBOOL bMarkRun = 0;     // a see-through water run that is gone over first, to mark the nearest of its points
        // The soft particles want the same depth copy the water wants - what lies behind the surface - so the copy is
        // made at the first run that needs EITHER, and one copy serves both for the rest of the frame.
        //
        // Sprites only (SetSprite). The level's own translucent surfaces are painted ON the geometry - a sign on a
        // wall, a pane in its frame - so what is behind them is a hand away, and a fade that measures that distance
        // would wipe them out. Measured: with every translucent run softened, the MEDICAL sign in the UNATCO corridor
        // and the medbay window both went dim. And a see-through MESH lies where it is meant to as well: the laser
        // sight's spot on its wall, a ring on the water, a hologram standing at a desk.
        // The sprite's own mark and not the surface kind: the kinds are kept only where the stencil carries them
        // (the shadows), and the fade is not the shadows' to give.
        const UBOOL bSoftMode = (SoftFade > 0.0f && Mode == BLEND_Translucent && Runs[i].bSprite) ? 1 : 0;
        if (Mode == BLEND_Water || Mode == BLEND_Fall || Mode == BLEND_SludgeMirror || Mode == BLEND_GlassMirror ||
            bSoftMode)
        {
            // Everything behind any water surface is drawn before the first one (docs/meres-11), so one copy there
            // serves the whole frame - and no water surface ever finds another one in its copy. The copy may draw and
            // bind targets of its own (the MSAA depth resolve): everything this pass had set, set again, as after the
            // depth clear above.
            UBOOL bCopiedHere = 0;
            if (!bWaterTried)
            {
                bWaterTried  = 1;
                bCopiedHere  = 1;
                // The splashes the camera is under go in first: the copy below then holds them, and the surface shows
                // them bent, as it shows everything above it.
                LaySplashes(1, -1);
                bWaterCopied = (Switch && (Pipelines[PipeSet][BLEND_Water] || Mode == BLEND_SludgeMirror ||
                                           Mode == BLEND_GlassMirror || bSoftMode))
                               ? Switch->CopyForWater(Cmd, Dsv) : 0;
                if (bWaterCopied)
                {
                    CopyAtOrder   = OrderAt;
                    CopyMode      = Mode;
                    bCopySoft     = bSoftMode;
                    bCopyCounting = 1;
                }
            }
            // A water surface whose copies were made for a pane's mirror layer: the panes laid on the picture since
            // are behind the water as often as not, and from the older copies it would not show them. Made again for
            // the frame's first water surface, as for a fall's sheet below.
            else if (Mode == BLEND_Water && bWaterCopied && CopyMode == BLEND_GlassMirror && !bWaterAfterCopy &&
                     OthersAfterCopy > 0)
            {
                bCopiedHere = 1;
                if (Switch->CopyForWaterAgain(Cmd, Dsv))
                {
                    OthersAfterCopy = 0;
                    CopiesAgain++;
                }
            }
            // A fall's sheet makes its whole pixel from the copy (PSFall), so whatever was laid on the picture after
            // the copies were made is not under it, and is wiped out where the sheet stands in front: the sludge's
            // mirror layer goes on ahead of the sheets, and drawn from the older copy the fall shows the bare surface
            // through itself. The copies are made again for the sheet. Sheets following one another share them -
            // crossed, they are one column.
            else if (Mode == BLEND_Fall && bWaterCopied && (bWaterAfterCopy || OthersAfterCopy > 0))
            {
                bCopiedHere = 1;
                if (Switch->CopyForWaterAgain(Cmd, Dsv))
                {
                    bWaterAfterCopy = 0;
                    OthersAfterCopy = 0;
                    CopiesAgain++;
                }
            }
            // The rays through the 64-bit host (AskRays): asked once for each stretch of the flush that one view's
            // depth serves - up to where the world ends, the depth is cleared or the targets change - at the
            // stretch's first run on rays, for every such run of it. The list comes back with nothing set on it.
            if ((Mode == BLEND_Water || Mode == BLEND_SludgeMirror || Mode == BLEND_GlassMirror) && bRtHosted &&
                bWaterCopied && Switch && WaterTable >= 0 && OrderAt >= AskedTo)
            {
                AskedTo = OrderAt + 1;
                while (AskedTo < NumOrder && !Runs[RunOrder[AskedTo]].bWorldEnd &&
                       !Runs[RunOrder[AskedTo]].bClearDepthFirst && !Runs[RunOrder[AskedTo]].bTargetSwitch)
                    AskedTo++;
                bRtWater    = AskRays(OrderAt, AskedTo);
                bCopiedHere = 1;
            }
            if (bCopiedHere)
            {
                Cmd->SetDescriptorHeaps(1, Heaps);
                Cmd->SetGraphicsRootSignature(RootSignature.Get());
                bRtRootSet = 0;
                Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
                Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                bTopologyPatches = 0;
                Cmd->IASetVertexBuffers(0, 1, &View);
                CurrentMode     = -1;
                CurrentNoSmooth = -1;
                CurrentPom      = 1;
                CurrentStencil  = -1;
                CurrentSrv      = -1;
                CurrentLightSrv = -1;
            }
        }
        if (Mode == BLEND_Water)
        {
            // Without the copies, or the lamps' constants, the water is what it always was: a translucent surface.
            if (!bWaterCopied || !WaterLamps)
                Mode = BLEND_Translucent;
            else
            {
                WaterMissed = Max(WaterMissed, OthersAfterCopy);
                if (bRtWater && RtWaterPipe(PipeSet))
                {
                    bRtRun = 1;
                    UseRtRoot();
                }
                // The water's own depth, for the run's pixel shader to write where its surface is seen
                // (FTargetSwitch::WaterSurfaceView) - the constant says whether it may.
                UBOOL     bSurface    = 0;
                const INT SurfaceView = Switch ? Switch->WaterSurfaceView(Cmd, bSurface) : -1;
                FLOAT     WaterRun[WATER_CONSTANTS];
                for (INT k = 0; k < WATER_CONSTANTS; k++)
                    WaterRun[k] = Runs[i].Water[k];
                // Gone over first where there is a pipeline for it and the depth can be written (MakeWaterMark): 2
                // tells the drawing that the depth holds the nearest of the run's points, to keep that one alone.
                ID3D12PipelineState* const MarkPso = bRtRun ? RtWaterMarkPipelines[PipeSet].Get()
                                                            : WaterMarkPipelines[PipeSet].Get();
                bMarkRun = (bSurface && SurfaceView >= 0 && !bWaterMarkOff && MarkPso != nullptr) ? 1 : 0;
                WaterRun[WATER_SURFACE_CONSTANT] = (bSurface && SurfaceView >= 0) ? (bMarkRun ? 2.0f : 1.0f) : 0.0f;
                if (SurfaceView >= 0)
                    Cmd->SetGraphicsRootDescriptorTable(bRtRun ? ROOT_WATER_UAV_RT : ROOT_WATER_UAV,
                                                        Textures.GetGpuHandle(SurfaceView));
                Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(WaterTable));
                Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, WaterRun, 4);
                Cmd->SetGraphicsRootConstantBufferView(5, WaterLamps);
            }
        }
        if (Mode == BLEND_Fall)
        {
            if (!bWaterCopied || !Pipelines[PipeSet][BLEND_Fall])
                Mode = BLEND_Modulated;
            else
            {
                Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(WaterTable));
                Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, Runs[i].Water, 4);
            }
        }
        // The sludge and its fog map read the ripples and the wave field from the water's views, and the lamps - no copy.
        // RenderDevice hands them over only when they can be drawn (CanSludge); should that have changed since, the sludge
        // is drawn flat, and its fog map, whose coordinates are not where a flat fog map's are, not at all.
        if (Mode == BLEND_Sludge || Mode == BLEND_SludgeFog)
        {
            if (!CanSludge())
            {
                if (Mode == BLEND_SludgeFog)
                    continue;
                Mode = BLEND_Opaque;
            }
            else
            {
                Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(WaterTable));
                Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, Runs[i].Water, 4);
                Cmd->SetGraphicsRootConstantBufferView(5, WaterLamps);
            }
        }
        // The sludge's mirror layer: only over a sludge that was drawn as one (CanSludge) - drawn flat it has no patches for
        // the layer to lie on - with the frame's copy of the picture where it was made, said in the layer's constants
        // (SludgeMirror), and on rays where the RT water is.
        if (Mode == BLEND_SludgeMirror)
        {
            if (!CanSludge() || !Pipelines[PipeSet][BLEND_SludgeMirror])
                continue;
            if (bRtWater && RtSludgeMirrorPipe(PipeSet))
            {
                bRtRun = 1;
                UseRtRoot();
            }
            FLOAT WaterRun[WATER_CONSTANTS];
            for (INT k = 0; k < WATER_CONSTANTS; k++)
                WaterRun[k] = Runs[i].Water[k];
            WaterRun[WATER_SURFACE_CONSTANT] = bWaterCopied ? 1.0f : 0.0f;
            Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(WaterTable));
            Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, WaterRun, 4);
            Cmd->SetGraphicsRootConstantBufferView(5, WaterLamps);
        }
        // The glass's mirror layer: over the frame's copy of the picture - without the copy, or without the layer's
        // pipeline, there is no layer, and the pane stands as the level drew it - and on rays where the frame traces
        // them. The rays' fallback light reads the lamps' block: the block of zeros, as no lamp of the water's is the
        // pane's.
        if (Mode == BLEND_GlassMirror)
        {
            if (!bWaterCopied || WaterTable < 0 || !Pipelines[PipeSet][BLEND_GlassMirror])
                continue;
            const D3D12_GPU_VIRTUAL_ADDRESS Zeros = (HeldFrame >= 0 && HeldBuffers[HeldFrame])
                                                    ? HeldBuffers[HeldFrame]->GetGPUVirtualAddress() : 0;
            if (bRtWater && RtGlassMirrorPipe(PipeSet) && Zeros)
            {
                bRtRun = 1;
                UseRtRoot();
                Cmd->SetGraphicsRootConstantBufferView(5, Zeros);
            }
            // A pane's layer is drawn on rays alone: without them the pane stands as the level drew it. A mirror's
            // whole image (its constant 31) keeps the picture's copy to fall back on.
            else if (Runs[i].Water[31] <= 0.5f)
                continue;
            else if (bRtRootSet)
            {
                // Not on rays after all, and the rays' root signature is the one set: the pass's own back first.
                Cmd->SetGraphicsRootSignature(RootSignature.Get());
                Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
                bRtRootSet      = 0;
                CurrentMode     = -1;
                CurrentNoSmooth = -1;
                CurrentPom      = 1;
                CurrentSrv      = -1;
                CurrentLightSrv = -1;
            }
            Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(WaterTable));
            Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, Runs[i].Water, 4);
        }
        // An effect's body (BLEND_Body): its own numbers, by the pass's own root signature - it reads no rays.
        if (Mode == BLEND_Body)
        {
            if (!Pipelines[PipeSet][BLEND_Body])
                continue;
            if (bRtRootSet)
            {
                Cmd->SetGraphicsRootSignature(RootSignature.Get());
                Cmd->SetGraphicsRoot32BitConstants(0, 6, Constants, 0);
                bRtRootSet      = 0;
                CurrentMode     = -1;
                CurrentNoSmooth = -1;
                CurrentPom      = 1;
                CurrentSrv      = -1;
                CurrentLightSrv = -1;
            }
            Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, Runs[i].Water, 4);
        }
        // Geometry=Tessellation: without its pipelines such a run is not drawn at all - the flat surface drawn after it
        // (DrawComplexSurface) then stands alone, as it would with the relief off.
        const UBOOL bTessRun = (Runs[i].bTess && ((Mode == BLEND_Opaque && TessPipelines[PipeSet]) ||
                                                  (Mode == BLEND_Modulated && TessModPipelines[PipeSet]) ||
                                                  (Mode == BLEND_Fog && TessFogPipelines[PipeSet]))) ? 1 : 0;
        if (Runs[i].bTess && !bTessRun)
            continue;
        // The patches read what stands above them (DSRelief, ReliefClear): the covers are made before a view's first
        // patches, once a view - the depth drawn so far, and the rest of that view's covers drawn into it - and every
        // patch run
        // binds them, or the view of nothing where none were made. With no view of nothing to bind, the patches are not
        // drawn at all, and the flat surface after them stands alone.
        if (bTessRun)
        {
            if (CoverNone < 0)
                continue;
            if (!bCoverTried)
            {
                bCoverTried = 1;
                if (Switch && CoverPipeline && CoverView >= 0 && Switch->BeginReliefCovers(Cmd))
                {
                    INT       Modulated = 0;
                    const INT Covers    = DrawReliefCovers(Cmd, Textures, OrderAt + 1, NumOrder, Modulated);
                    Switch->EndReliefCovers(Cmd, Dsv);
                    bCoverMade = 1;
                    static INT Told = 0;
                    if (Told < 3)
                    {
                        Told++;
                        RdxLog(TEXT("relief: the covers made - the depth drawn so far, and %i more runs drawn into it"),
                               Covers);
                    }
                    // Said now and then while there are any: which views have a modulated sheet among the covers.
                    static INT CoverFrames = 0;
                    if (Modulated > 0 && (CoverFrames++ % 300) == 0)
                        RdxLog(TEXT("relief: %i of the frame's %i covers are the level's modulated surfaces"),
                               Modulated, Covers);
                    // The covers drew with a pipeline of their own, and set a texture and the filtering: all set again.
                    CurrentMode      = -1;
                    CurrentSrv       = -1;
                    CurrentNoSmooth  = -1;
                    bTopologyPatches = 0;
                }
            }
            Cmd->SetGraphicsRootDescriptorTable(6, Textures.GetGpuHandle(bCoverMade ? CoverView : CoverNone));
        }
        // The relief's own numbers ride in the water's constants (the relief's four go in after them, below), and its
        // lamps in the water's lamps' place.
        if (Runs[i].bRelief && Mode != BLEND_Water)
        {
            Cmd->SetGraphicsRoot32BitConstants(3, WATER_CONSTANTS, Runs[i].Water, 4);
            D3D12_GPU_VIRTUAL_ADDRESS Block = Runs[i].ReliefBlock ? Runs[i].ReliefBlock : ReliefLamps;
            if (!Block && HeldFrame >= 0 && HeldBuffers[HeldFrame])
                Block = HeldBuffers[HeldFrame]->GetGPUVirtualAddress();      // the zeros at its start
            if (Block)
                Cmd->SetGraphicsRootConstantBufferView(5, Block);
        }

        // The four spare numbers carry the relief on an opaque run and the soft fade on a translucent one. A run is
        // never both - relief is for the level's solid surfaces, the fade for what is drawn see-through over them.
        FLOAT Spare[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (Runs[i].bPom)
        {
            for (INT k = 0; k < 4; k++)
                Spare[k] = Runs[i].Pom[k];
            Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(Runs[i].PomTable));
        }
        else if (bSoftMode && bWaterCopied && WaterTable >= 0 && Mode == BLEND_Translucent)
        {
            Spare[0] = SoftFade;
            Spare[1] = RdxMath::PomFlag_Soft;
            // The depth behind this surface sits in the WATER's table (t3), so that is what has to be bound - a relief
            // run before this one would otherwise have left its own height map there.
            Cmd->SetGraphicsRootDescriptorTable(4, Textures.GetGpuHandle(WaterTable));
        }
        // A water run's own constants, written above, fill these four places themselves (WaterSpare: its mirror
        // switch) - left as they are, and not zeros for the next run.
        if (Mode == BLEND_Water || Mode == BLEND_Fall || Mode == BLEND_Sludge || Mode == BLEND_SludgeFog ||
            Mode == BLEND_SludgeMirror || Mode == BLEND_GlassMirror || Mode == BLEND_Body)
            CurrentPom = 1;
        else if (Spare[1] != 0.0f || CurrentPom || Runs[i].bRelief)
        {
            // The last run carried something here and this one may not: the four numbers have to be written either
            // way, or the shader would read the previous surface's and put a relief on a wall that has no height map.
            Cmd->SetGraphicsRoot32BitConstants(3, 4, Spare, 4 + WATER_CONSTANTS - 4);
            CurrentPom = (Spare[1] != 0.0f) ? 1 : 0;
        }

        // The water's triangles go in as patches, to be cut and lifted (DSWater) - the sludge's and its fog map's too - and
        // so do the relief's (DSRelief); everything else as triangles.
        const UBOOL bPatches = (Mode == BLEND_Water || Mode == BLEND_Sludge || Mode == BLEND_SludgeFog ||
                                Mode == BLEND_SludgeMirror || bTessRun) ? 1 : 0;
        if (bPatches != bTopologyPatches)
        {
            Cmd->IASetPrimitiveTopology(bPatches ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                 : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            bTopologyPatches = bPatches;
        }

        // The RT water's pipeline goes by a mode of its own, past the blend modes, the relief's three by the next, and the
        // sludge mirror layer's on rays by the one after them.
        const INT PipeMode = bRtRun ? ((Mode == BLEND_SludgeMirror) ? (INT)BLEND_MAX + 4
                                       : (Mode == BLEND_GlassMirror) ? (INT)BLEND_MAX + 5 : (INT)BLEND_MAX)
                                    : (bTessRun ? (INT)BLEND_MAX + ((Mode == BLEND_Opaque)    ? 1
                                                                    : (Mode == BLEND_Modulated) ? 2 : 3)
                                                : Mode);
        ID3D12PipelineState* const Pso =
            bRtRun   ? ((Mode == BLEND_SludgeMirror) ? RtSludgeMirrorPipe(PipeSet)
                        : (Mode == BLEND_GlassMirror) ? RtGlassMirrorPipe(PipeSet)
                                                      : RtWaterPipe(PipeSet)) :
            bTessRun ? ((Mode == BLEND_Opaque)    ? TessPipelines[PipeSet].Get()
                        : (Mode == BLEND_Modulated) ? TessModPipelines[PipeSet].Get()
                                                    : TessFogPipelines[PipeSet].Get()) :
                       Pipelines[PipeSet][Mode].Get();
        if (PipeMode != CurrentMode)
        {
            Cmd->SetPipelineState(Pso);
            CurrentMode = PipeMode;
        }

        if ((INT)Runs[i].bNoSmooth != CurrentNoSmooth)
        {
            CurrentNoSmooth = Runs[i].bNoSmooth ? 1 : 0;
            Cmd->SetGraphicsRoot32BitConstant(3, (UINT)CurrentNoSmooth, 0);
        }

        if (Runs[i].SrvIndex != CurrentSrv)
        {
            Cmd->SetGraphicsRootDescriptorTable(1, Textures.GetGpuHandle(Runs[i].SrvIndex));
            CurrentSrv = Runs[i].SrvIndex;
        }
        const INT LightSrv = Runs[i].LightSrv >= 0 ? Runs[i].LightSrv : Textures.GetWhiteIndex();
        if (LightSrv != CurrentLightSrv)
        {
            Cmd->SetGraphicsRootDescriptorTable(2, Textures.GetGpuHandle(LightSrv));
            CurrentLightSrv = LightSrv;
        }
        if (bStencilKinds && Runs[i].Kind != CurrentStencil)
        {
            Cmd->OMSetStencilRef((UINT)Runs[i].Kind);
            CurrentStencil = Runs[i].Kind;
        }
        // The flashlight's block: the frame's - all of the light, or a see-through surface's share - for a run that
        // takes it, the zeros for one that does not. Set for every run on the pass's own root signature: whatever
        // was bound before it, this is bound now.
        if (!bRtRun)
        {
            const INT How = Runs[i].Beam;
            Cmd->SetGraphicsRootConstantBufferView(ROOT_BEAM, (How > 0 && BeamOn[How - 1]) ? BeamOn[How - 1]
                                                                                           : BeamOff());
            // And the mirrors' map, which the block's mirrors are looked up in.
            if (BounceView >= 0)
                Cmd->SetGraphicsRootDescriptorTable(ROOT_BOUNCE, Textures.GetGpuHandle(BounceView));
        }
        if (bDredMarkers)
        {
            WCHAR Label[96];
            const INT Len = swprintf_s(Label, 96, L"run %d: mode %d%s%s, %d vertices from %d", i, Mode,
                                       bTessRun ? L", tessellated" : L"", bRtRun ? L", RT water" : L"",
                                       Runs[i].Count, Runs[i].Start);
            if (Len > 0)
                Cmd->SetMarker(0, Label, (UINT)((Len + 1) * sizeof(WCHAR)));
        }
        // The see-through water writes no depth: where its lifted surface stands in front of itself - a crest before
        // the trough behind it - whatever was drawn later would show. So such a run goes twice: first only to mark
        // the nearest of its points at every pixel in the water's own depth, then drawn, each pixel keeping that
        // point alone (the shader's WaterSurface).
        // Not a third time where the run's rays were asked of the 64-bit host: its points were marked for the
        // asking (AskRays), through the very stages this drawing goes through.
        if (bMarkRun && !(bRtRun && bRtHosted && OrderAt < AskedTo && RtWaterAskMarkPipelines[PipeSet]))
        {
            Cmd->SetPipelineState(bRtRun ? RtWaterMarkPipelines[PipeSet].Get() : WaterMarkPipelines[PipeSet].Get());
            Cmd->DrawInstanced(Runs[i].Count, 1, Runs[i].Start, 0);
            Switch->WaterSurfaceSettle(Cmd);
            Cmd->SetPipelineState(Pso);
        }
        Cmd->DrawInstanced(Runs[i].Count, 1, Runs[i].Start, 0);
        if (bCopyCounting && (Mode == BLEND_Opaque || Mode == BLEND_TranslucentOccluding || Mode == BLEND_Sludge))
            DepthAfterCopy[(Runs[i].Kind == 1) ? 1 : 0]++;
        if (bCopyCounting && Mode == BLEND_Water)
            bWaterAfterCopy = 1;
        else if (bCopyCounting && Mode != BLEND_Fall)
            OthersAfterCopy++;
        // Not the relief's patches: their vertices are in view space, not on the screen, so there is no area to add.
        if (bModeStats && !bTessRun && (INT)Runs[i].Mode >= 0 && (INT)Runs[i].Mode < BLEND_MAX)
        {
            const INT M = (INT)Runs[i].Mode;
            const INT K = (Runs[i].Kind == 1) ? 1 : 0;
            DOUBLE    A = 0.0;
            for (INT v = Runs[i].Start; v + 2 < Runs[i].Start + Runs[i].Count; v += 3)
            {
                const FSolidVertex& P0 = Filled[v];
                const FSolidVertex& P1 = Filled[v + 1];
                const FSolidVertex& P2 = Filled[v + 2];
                const DOUBLE Cr = (DOUBLE)(P1.X - P0.X) * (DOUBLE)(P2.Y - P0.Y) - (DOUBLE)(P2.X - P0.X) * (DOUBLE)(P1.Y - P0.Y);
                A += (Cr < 0.0) ? -Cr : Cr;
            }
            GModeRuns[M][K]++;
            GModeTris[M][K] += Runs[i].Count / 3;
            GModeArea[M][K] += 0.5 * A;
        }
    }
    // A last chance for the depth's copy, when no run above asked for one: the world's runs are over, and the pass
    // still holds the targets it drew with - the state the copy expects. CopyForWater decides for itself whether
    // anyone wants it, and refuses before touching a resource when nobody does. Whether this chance ever makes the
    // copy is not known - the copy the smoke and the light shafts read is made where the scene is resolved - so it
    // is counted, and said at the first time and at every six-hundredth: a call that never makes it can go.
    if (!bWaterCopied && Switch)
    {
        static INT Asked = 0, Made = 0;
        bWaterCopied = Switch->CopyForWater(Cmd, Dsv);
        Made += bWaterCopied ? 1 : 0;
        if (Asked++ % 600 == 0)
            RdxLog(TEXT("the solid pass's last chance for the depth's copy: asked %i times, made the copy %i times"),
                   Asked, Made);
    }
    {
        // What the depth copy missed (see DepthAfterCopy): the first frames it happens in, then one in 300.
        static INT Missed = 0, Frames = 0;
        Frames++;
        if (CopyAtOrder >= 0 && DepthAfterCopy[0] + DepthAfterCopy[1] > 0 && (Missed++ < 10 || (Frames % 300) == 0))
            RdxLog(TEXT("the depth copy was made at run %i of %i (mode %i%s); after it %i of the level's runs and %i "
                        "of the actors' still wrote the depth before the world ended - the smoke, the waterfalls and "
                        "the soft particles do not see them"),
                   CopyAtOrder, NumOrder, CopyMode, bCopySoft ? TEXT(", soft particles") : TEXT(""),
                   DepthAfterCopy[0], DepthAfterCopy[1]);
    }
    {
        // What the picture's copy missed, and what was done about it: the first frames either happens in, then one
        // in 300.
        static INT Told = 0, Frames = 0;
        Frames++;
        if ((WaterMissed > 0 || CopiesAgain > 0) && (Told++ < 10 || (Frames % 300) == 0))
            RdxLog(TEXT("the picture's copy was made at run %i of %i (mode %i%s); %i runs were laid on the picture "
                        "after it and before a water surface, which draws from the copy and does not show them; made "
                        "again %i times for a fall's sheet"),
                   CopyAtOrder, NumOrder, CopyMode, bCopySoft ? TEXT(", soft particles") : TEXT(""), WaterMissed,
                   CopiesAgain);
    }
    if (bModeStats && ++GModeFrames >= 150)
    {
        const DOUBLE Screen = (DOUBLE)Max(Width, 1) * (DOUBLE)Max(Height, 1) * (DOUBLE)GModeFrames;
        const DOUBLE F      = (DOUBLE)GModeFrames;
        RdxLog(TEXT("draws by blend mode (%i frames, per frame; draws / triangles / screen covered, BSP | actor): "
                    "0 opaque %.0f/%.0f/%.2f | %.0f/%.0f/%.2f; 1 translucent %.0f/%.0f/%.2f | %.0f/%.0f/%.2f; "
                    "2 modulated %.0f/%.0f/%.2f | %.0f/%.0f/%.2f; 3 no depth %.0f/%.0f/%.2f | %.0f/%.0f/%.2f; "
                    "4 depth-writing translucent %.0f/%.0f/%.2f | %.0f/%.0f/%.2f"), GModeFrames,
               GModeRuns[0][0] / F, GModeTris[0][0] / F, GModeArea[0][0] / Screen, GModeRuns[0][1] / F, GModeTris[0][1] / F, GModeArea[0][1] / Screen,
               GModeRuns[1][0] / F, GModeTris[1][0] / F, GModeArea[1][0] / Screen, GModeRuns[1][1] / F, GModeTris[1][1] / F, GModeArea[1][1] / Screen,
               GModeRuns[2][0] / F, GModeTris[2][0] / F, GModeArea[2][0] / Screen, GModeRuns[2][1] / F, GModeTris[2][1] / F, GModeArea[2][1] / Screen,
               GModeRuns[3][0] / F, GModeTris[3][0] / F, GModeArea[3][0] / Screen, GModeRuns[3][1] / F, GModeTris[3][1] / F, GModeArea[3][1] / Screen,
               GModeRuns[4][0] / F, GModeTris[4][0] / F, GModeArea[4][0] / Screen, GModeRuns[4][1] / F, GModeTris[4][1] / F, GModeArea[4][1] / Screen);
        for (INT m = 0; m < BLEND_MAX; m++)
            for (INT k = 0; k < 2; k++)
            {
                GModeRuns[m][k] = 0;
                GModeTris[m][k] = 0;
                GModeArea[m][k] = 0.0;
            }
        GModeFrames = 0;
    }
}

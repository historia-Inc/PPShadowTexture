// Reads UE 5.8 / 5.9's own shadow data for one view in the PostOpaque hook and turns it into a screen space mask.
//
// Sources (all renderer-private, so this file is tied to UE 5.8 and 5.9):
// - main directional light: CSM cascades (FProjectedShadowInfo, one atlas) or its VSM clipmap
// - local lights (point / spot / rect): VSM, or the whole scene shadow map (2D atlas tile for spot,
//   one-pass cube map for point and rect)
// Passes: [Local VSM | clear] -> one pass per shadow-mapped local light -> Compose (writes R8G8 mask).
#include "PPShadowTextureMask.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "LightSceneInfo.h"
#include "LightSceneProxy.h"
#include "Misc/EngineVersionComparison.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "SceneManagement.h"
#include "ScenePrivate.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "SceneTexturesConfig.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterUtils.h"
#include "ShadowRendering.h"
#include "StereoRendering.h"
#include "VirtualShadowMaps/VirtualShadowMapArray.h"
#include "VirtualShadowMaps/VirtualShadowMapCacheManager.h"
#include "VirtualShadowMaps/VirtualShadowMapClipmap.h"

DEFINE_LOG_CATEGORY_STATIC(LogPPShadowTexture, Log, All);

namespace PPShadowTexture
{
const FName TextureName(TEXT("PPShadowTexture"));

namespace
{
constexpr int32 MaxCascades = 16;
constexpr int32 MaxArrayLights = 64;	// local lights handled in one loop: VSM-shadowed and unshadowed ones

TAutoConsoleVariable<int32> CVarEnable(TEXT("r.PPShadowTexture.Enable"), 1,
	TEXT("Publish UE's shadows as the UserSceneTexture 'PPShadowTexture' (R = main directional light, G = local lights)."),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<int32> CVarLog(TEXT("r.PPShadowTexture.Log"), 0,
	TEXT("Log, per view and frame, which shadows PPShadowTexture found."), ECVF_RenderThreadSafe);

// G weights each local light's shadow by the direct light it brings to the pixel. Per pixel, the local passes sum
// E = unshadowed irradiance of the local lights and Lost = irradiance their shadows block; Compose then writes
// G = 1 - Lost / (sun irradiance * R + E).
enum class EMode : int32
{
	ComposeNone,	// R = 1
	ComposeCSM,		// R = directional cascades
	ComposeVSM,		// R = directional VSM clipmap
	LocalArray,		// E, Lost = sums over the array lights, all unshadowed (Lost = 0); first writer
	LocalArrayVSM,	// E, Lost = sums over the array lights, some VSM-shadowed; first writer
	LocalSpot,		// E, Lost += one spot light with a 2D shadow map
	LocalCube,		// E, Lost += one point / rect light with a cube shadow map
	Num
};

const TCHAR* GetModeName(EMode Mode)
{
	static const TCHAR* Names[] = { TEXT("Compose(None)"), TEXT("Compose(CSM)"), TEXT("Compose(VSM)"),
		TEXT("Local(Array)"), TEXT("Local(ArrayVSM)"), TEXT("Local(Spot)"), TEXT("Local(Cube)") };
	return Names[int32(Mode)];
}

bool UsesVirtualShadowMaps(EMode Mode)
{
	return Mode == EMode::ComposeVSM || Mode == EMode::LocalArrayVSM;
}

class FPPShadowMaskCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPPShadowMaskCS);
	// FVirtualShadowMapUniformParameters' metadata is not exported from the Renderer module, so it cannot
	// be a member of FParameters. Bind it by name next to the struct and set its RHI by hand.
	SHADER_ROOT_PARAMETERS_SIGNATURE()

	class FModeDim : SHADER_PERMUTATION_INT("PPST_MODE", int32(EMode::Num));
	using FPermutationDomain = TShaderPermutationDomain<FModeDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
		// Read by VirtualShadowMapScreenRayCast (scene depth).
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PPST_SceneDepth)
		SHADER_PARAMETER(FIntPoint, PPST_ViewMin)
		SHADER_PARAMETER(FIntPoint, PPST_ViewSize)
		SHADER_PARAMETER(FIntPoint, PPST_OutputSize)
		// 2D shadow maps: directional cascades, or one spot light (slot 0)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PPST_ShadowDepth)
		SHADER_PARAMETER_SAMPLER(SamplerState, PPST_ShadowDepthSampler)
		SHADER_PARAMETER(FVector4f, PPST_ShadowBufferSize)
		SHADER_PARAMETER_ARRAY(FMatrix44f, PPST_TranslatedWorldToShadowClip, [MaxCascades])	// z scaled to the stored depth range
		SHADER_PARAMETER_ARRAY(FVector4f, PPST_ShadowTileMinMax, [MaxCascades])	// the tile's UV rect in the atlas
		SHADER_PARAMETER_ARRAY(FVector4f, PPST_ShadowParams, [MaxCascades])	// x split far (view depth), y transition scale, z fade alpha, w receiver bias (0..1)
		SHADER_PARAMETER(FVector3f, PPST_DirectionalToLight)
		SHADER_PARAMETER(int32, PPST_NumShadows)
		SHADER_PARAMETER(uint32, PPST_CascadeContinuationMask)	// bit i: slot i is the dynamic-only half of slot i - 1's cascade
		SHADER_PARAMETER(float, PPST_ShadowSharpen)
		// one-pass cube shadow map (point / rect)
		SHADER_PARAMETER_RDG_TEXTURE(TextureCube, PPST_CubeShadowDepth)
		SHADER_PARAMETER_SAMPLER(SamplerComparisonState, PPST_CubeShadowSampler)
		SHADER_PARAMETER_ARRAY(FMatrix44f, PPST_CubeViewProjection, [6])
		SHADER_PARAMETER(FVector4f, PPST_CubeParams)	// x 1/resolution, y depth bias, z slope bias, w max slope bias
		SHADER_PARAMETER(float, PPST_CubeFade)
		// CubemapHardwarePCF (ShadowProjectionCommon.ush) reads this global instead of its argument for its disc taps.
		SHADER_PARAMETER(float, InvShadowmapResolution)
		// local lights: one for Spot / Cube, up to MaxArrayLights for Local(Array)
		SHADER_PARAMETER_ARRAY(FVector4f, PPST_LightPositionAndInvRadius, [MaxArrayLights])	// translated world
		SHADER_PARAMETER_ARRAY(FVector4f, PPST_LightDirectionAndCosOuter, [MaxArrayLights])
		SHADER_PARAMETER_ARRAY(FVector4f, PPST_LightEnergy, [MaxArrayLights])	// x luminance * diffuse scale, y spot 1/(cos inner - cos outer), z falloff exponent, w source radius (rect: half width)
		SHADER_PARAMETER_ARRAY(FVector4f, PPST_LightTangentAndLength, [MaxArrayLights])	// xyz tangent, w source length (rect: half height)
		SHADER_PARAMETER_ARRAY(FIntVector4, PPST_LightTypeAndVsmId, [MaxArrayLights])	// x type, y VSM id (-1: unshadowed), z inverse squared
		SHADER_PARAMETER(int32, PPST_NumLights)
		SHADER_PARAMETER(int32, PPST_DirectionalVsmId)
		SHADER_PARAMETER(float, PPST_DirectionalEnergy)	// main directional light: luminance * diffuse scale (lux)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, PPST_LocalEnergy)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, PPST_LocalLost)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PPST_LocalEnergyOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PPST_LocalLostOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, PPST_MaskOutput)
	END_SHADER_PARAMETER_STRUCT()

	FPPShadowMaskCS() = default;
	FPPShadowMaskCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) : FGlobalShader(Initializer)
	{
		BindForLegacyShaderParameters<FParameters>(this, Initializer.PermutationId, Initializer.ParameterMap, false);
		VirtualShadowMapBuffer.Bind(Initializer.ParameterMap, TEXT("VirtualShadowMap"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		if (!IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) || IsMobilePlatform(Parameters.Platform))
		{
			return false;
		}
		const FPermutationDomain Permutation(Parameters.PermutationId);
		return !UsesVirtualShadowMaps(EMode(Permutation.Get<FModeDim>())) || DoesPlatformSupportVirtualShadowMaps(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("PPST_MAX_CASCADES"), MaxCascades);
		OutEnvironment.SetDefine(TEXT("PPST_MAX_LIGHTS"), MaxArrayLights);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 9, 0)
		// UE 5.9's VSM page table addressing needs this define, which FVirtualShadowMapArray::SetShaderDefines sets
		// (not exported). Single-page VSMs take the first page table entries: the smallest power of two of entries whose
		// level 0 pages outnumber VSM_MAX_SINGLE_PAGE_SHADOW_MAPS. The define is the log2 of that entry count.
		const uint32 SinglePageEntries = FMath::RoundUpToPowerOfTwo(
			uint32(VSM_MAX_SINGLE_PAGE_SHADOW_MAPS) / (uint32(VSM_LEVEL0_DIM_PAGES_XY) * uint32(VSM_LEVEL0_DIM_PAGES_XY)) + 1u);
		OutEnvironment.SetDefine(TEXT("VSM_LOG2_FULL_SHADOW_MAP_OFFSET"), *FString::Printf(TEXT("%uu"), FMath::FloorLog2(SinglePageEntries)));
#endif
	}

	LAYOUT_FIELD(FShaderUniformBufferParameter, VirtualShadowMapBuffer);
};
IMPLEMENT_GLOBAL_SHADER(FPPShadowMaskCS, "/Plugin/PPShadowTexture/Private/PPShadowTexture.usf", "MainCS", SF_Compute);

BEGIN_SHADER_PARAMETER_STRUCT(FPPShadowMaskPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FPPShadowMaskCS::FParameters, Shader)
	// RDG resources referenced by the VSM uniform buffer, declared so RDG keeps them alive and in SRV state.
	RDG_BUFFER_ACCESS_ARRAY(VirtualShadowMapBuffers)
	RDG_TEXTURE_ACCESS_ARRAY(VirtualShadowMapTextures)
END_SHADER_PARAMETER_STRUCT()

struct FLocalShadowMap
{
	const FLightSceneInfo* Light = nullptr;
	const FProjectedShadowInfo* Shadow = nullptr;
};

struct FArrayLight
{
	const FLightSceneInfo* Light = nullptr;
	int32 VirtualShadowMapId = INDEX_NONE;	// INDEX_NONE: the light adds irradiance but no shadow
};

float GetRenderThreadFloat(const TCHAR* Name, float Default)
{
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
	TConsoleVariableData<float>* Data = Variable ? Variable->AsVariableFloat() : nullptr;
	return Data ? Data->GetValueOnRenderThread() : Default;
}

/** r.Shadow.*ReceiverBias: the share of the transition size a surface facing away from the light gives up. */
float GetReceiverBias(const TCHAR* CVarName)
{
	return FMath::Clamp(GetRenderThreadFloat(CVarName, 0.9f), 0.0f, 1.0f);
}

FRDGTextureRef CreateMaskTexture(FRDGBuilder& GraphBuilder, FIntPoint Extent)
{
	return GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R8G8, FClearValueBinding::White,
		TexCreate_ShaderResource | TexCreate_UAV), TEXT("UST.PPShadowTexture"));
}

FPPShadowMaskPassParameters* AllocatePassParameters(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FMaskInputs& Inputs, FIntPoint Extent)
{
	auto* PassParameters = GraphBuilder.AllocParameters<FPPShadowMaskPassParameters>();
	FPPShadowMaskCS::FParameters& P = PassParameters->Shader;
	// AllocParameters does not zero scalars; every pass leaves some parameters unset (e.g. the sun's energy when there
	// is no main directional light), so start from zeros. Safe: the struct only holds raw pointers, PODs and a null
	// uniform buffer reference at this point.
	FMemory::Memzero(&P, sizeof(P));
	P.View = View.ViewUniformBuffer;
	P.Scene = View.GetSceneUniforms().GetBuffer(GraphBuilder);
	P.SceneTextures = Inputs.SceneTextures;
	P.PPST_SceneDepth = Inputs.SceneDepth;
	P.PPST_ViewMin = View.ViewRect.Min;
	P.PPST_ViewSize = View.ViewRect.Size();
	P.PPST_OutputSize = Extent;
	P.PPST_ShadowDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	P.PPST_CubeShadowSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp, 0, 0, 0, SCF_Less>::GetRHI();
	P.PPST_ShadowSharpen = 1.0f;
	P.PPST_CubeFade = 1.0f;
	P.PPST_DirectionalVsmId = INDEX_NONE;
	return PassParameters;
}

void DeclareVirtualShadowMapReads(const FRDGUniformBuffer& VirtualShadowMapUB, FPPShadowMaskPassParameters& PassParameters)
{
	TSet<FRDGBuffer*> Buffers;
	TSet<FRDGTexture*> Textures;
	VirtualShadowMapUB.GetParameters().Enumerate([&](FRDGParameter Parameter)
	{
		switch (Parameter.GetType())
		{
		case UBMT_RDG_BUFFER_SRV:
			if (FRDGBufferSRVRef SRV = Parameter.GetAsBufferSRV()) Buffers.Add(SRV->GetParent());
			break;
		case UBMT_RDG_TEXTURE_SRV:
		case UBMT_RDG_TEXTURE_NON_PIXEL_SRV:
			if (FRDGTextureSRVRef SRV = Parameter.GetAsTextureSRV()) Textures.Add(SRV->GetParent());
			break;
		case UBMT_RDG_TEXTURE:
			if (FRDGTextureRef Texture = Parameter.GetAsTexture()) Textures.Add(Texture);
			break;
		default:
			break;
		}
	});
	for (FRDGBuffer* Buffer : Buffers)
	{
		PassParameters.VirtualShadowMapBuffers.Emplace(Buffer, ERHIAccess::SRVCompute);
	}
	for (FRDGTexture* Texture : Textures)
	{
		PassParameters.VirtualShadowMapTextures.Emplace(Texture, ERHIAccess::SRVCompute);
	}
}

/** The VSM uniform buffer as RDG sees it, plus its RHI buffer, made external so our passes may use it. */
struct FVirtualShadowMapBinding
{
	FRDGUniformBuffer* UniformBuffer = nullptr;
	FRHIUniformBuffer* RHI = nullptr;
};

FVirtualShadowMapBinding MakeVirtualShadowMapBinding(FRDGBuilder& GraphBuilder, FRDGUniformBuffer* UniformBuffer)
{
	// The uniform buffer cannot be a declared pass parameter (its metadata is not exported), so RDG would not keep its
	// RHI buffer for our passes: calling GetRHI() there trips RDG validation and, without validation, can bind a buffer
	// RDG has already recycled. Converting it to external allocates the RHI buffer now and keeps it for the whole graph.
	FVirtualShadowMapBinding Binding;
	if (UniformBuffer)
	{
		Binding.UniformBuffer = UniformBuffer;
		Binding.RHI = GraphBuilder.ConvertToExternalUniformBuffer(UniformBuffer);
	}
	return Binding;
}

void AddMaskPass(FRDGBuilder& GraphBuilder, const FViewInfo& View, EMode Mode, FPPShadowMaskPassParameters* PassParameters,
	const FVirtualShadowMapBinding& VirtualShadowMap)
{
	FPPShadowMaskCS::FPermutationDomain Permutation;
	Permutation.Set<FPPShadowMaskCS::FModeDim>(int32(Mode));
	TShaderMapRef<FPPShadowMaskCS> Shader(View.ShaderMap, Permutation);
	const FIntVector Groups = FComputeShaderUtils::GetGroupCount(PassParameters->Shader.PPST_OutputSize, FIntPoint(8, 8));
	FRHIUniformBuffer* VirtualUB = UsesVirtualShadowMaps(Mode) ? VirtualShadowMap.RHI : nullptr;
	if (VirtualUB)
	{
		DeclareVirtualShadowMapReads(*VirtualShadowMap.UniformBuffer, *PassParameters);
	}
	// Not NeverCull: when no material reads the texture, RDG culls every pass of the mask.
	GraphBuilder.AddPass(RDG_EVENT_NAME("PPShadowTexture %s", GetModeName(Mode)), PassParameters, ERDGPassFlags::Compute,
		[PassParameters, Shader, Groups, VirtualUB](FRDGAsyncTask, FRHIComputeCommandList& RHICmdList)
	{
		// VirtualShadowMap is a static-slot uniform buffer. Set only that slot, so the static bindings RDG set
		// from the pass parameters (View, Scene) stay in place, and clear it again afterwards.
		static const FUniformBufferStaticSlot VirtualSlot =
			FUniformBufferStaticSlotRegistry::Get().FindSlotByName(TEXT("VirtualShadowMapUbSlot"));
		const bool bStaticSlot = VirtualUB && IsUniformBufferStaticSlotValid(VirtualSlot);
		if (bStaticSlot)
		{
			RHICmdList.SetStaticUniformBuffer(VirtualSlot, VirtualUB);
		}
		FComputeShaderUtils::Private::PrepareDispatch(RHICmdList, Shader, PassParameters->Shader);
		if (VirtualUB && Shader->VirtualShadowMapBuffer.IsBound())
		{
			FRHIBatchedShaderParameters& Batched = RHICmdList.GetScratchShaderParameters();
			SetUniformBufferParameter(Batched, Shader->VirtualShadowMapBuffer, VirtualUB);
			RHICmdList.SetBatchedShaderParameters(Shader.GetComputeShader(), Batched);
		}
		RHICmdList.DispatchComputeShader(Groups.X, Groups.Y, Groups.Z);
		FComputeShaderUtils::Private::AfterDispatch(RHICmdList, Shader);
		if (bStaticSlot)
		{
			RHICmdList.SetStaticUniformBuffer(VirtualSlot, nullptr);
		}
	});
}

/**
 * One 2D shadow map (a cascade or a spot light's tile of the atlas). The shadow projects positions given in its own
 * translated space (world + PreShadowTranslation); the shader receives view translated positions, so the matrix
 * first moves them between the two spaces. The shader maps the clip position into the tile's UV rect.
 */
void SetShadowMapSlot(FPPShadowMaskCS::FParameters& P, int32 Slot, const FViewInfo& View, int32 ViewIndex,
	const FProjectedShadowInfo& Shadow, float SplitFar, float TransitionSize, float ReceiverBias)
{
	const FVector ViewToShadowTranslation = Shadow.PreShadowTranslation - View.ViewMatrices.GetPreViewTranslation();
	const FMatrix DepthToStoredRange = FScaleMatrix(FVector(1.0, 1.0, Shadow.InvMaxSubjectDepth));
	P.PPST_TranslatedWorldToShadowClip[Slot] = FMatrix44f(
		FTranslationMatrix(ViewToShadowTranslation) * FMatrix(Shadow.TranslatedWorldToClipInnerMatrix) * DepthToStoredRange);

	// The tile starts after its border and spans the shadow's resolution.
	const FIntPoint Buffer = Shadow.RenderTargets.GetSize();
	const FVector2f InvBuffer(1.0f / Buffer.X, 1.0f / Buffer.Y);
	const FVector2f TileMin = FVector2f(Shadow.X + Shadow.BorderSize, Shadow.Y + Shadow.BorderSize) * InvBuffer;
	const FVector2f TileMax = TileMin + FVector2f(Shadow.ResolutionX, Shadow.ResolutionY) * InvBuffer;
	P.PPST_ShadowTileMinMax[Slot] = FVector4f(TileMin.X, TileMin.Y, TileMax.X, TileMax.Y);
	const float Fade = Shadow.FadeAlphas.IsValidIndex(ViewIndex) ? Shadow.FadeAlphas[ViewIndex] : 1.0f;
	// Same floor as FProjectedShadowInfo::ComputeTransitionSize.
	P.PPST_ShadowParams[Slot] = FVector4f(SplitFar, 1.0f / FMath::Max(TransitionSize, 0.00001f), Fade, ReceiverBias);
	P.PPST_ShadowBufferSize = FVector4f(Buffer.X, Buffer.Y, 1.0f / Buffer.X, 1.0f / Buffer.Y);
}

/** Luminance of the light's shader color (lux for directional lights; for local lights the units UE's falloff turns into lux). */
float GetDiffuseLuminance(const FLightRenderParameters& LightParameters)
{
	return LightParameters.Color.GetLuminance() * LightParameters.DiffuseScale;
}

void SetLocalLight(FPPShadowMaskCS::FParameters& P, int32 Slot, const FViewInfo& View, const FLightSceneInfo& Light, int32 VirtualShadowMapId)
{
	FLightRenderParameters LightParameters{};
	Light.Proxy->GetLightShaderParameters(LightParameters);
	const uint8 LightType = Light.Proxy->GetLightType();
	const FVector TranslatedPosition = LightParameters.WorldPosition + View.ViewMatrices.GetPreViewTranslation();
	P.PPST_LightPositionAndInvRadius[Slot] = FVector4f(FVector3f(TranslatedPosition), LightParameters.InvRadius);
	P.PPST_LightDirectionAndCosOuter[Slot] = FVector4f(LightParameters.Direction, LightParameters.SpotAngles.X);
	P.PPST_LightEnergy[Slot] = FVector4f(GetDiffuseLuminance(LightParameters), LightParameters.SpotAngles.Y, LightParameters.FalloffExponent,
		LightParameters.SourceRadius);
	P.PPST_LightTangentAndLength[Slot] = FVector4f(LightParameters.Tangent, LightParameters.SourceLength);
	// As FDeferredLightData::bInverseSquared.
	P.PPST_LightTypeAndVsmId[Slot] = FIntVector4(LightType, VirtualShadowMapId, LightParameters.FalloffExponent == 0.0f ? 1 : 0, 0);
}

/** True for the dynamic-only half of a cascade split by r.Shadow.CSMShadowDepthReuse.SeparateDynamic (UE 5.9). */
bool IsDynamicOnlyCascade(const FProjectedShadowInfo& Shadow)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 9, 0)
	return Shadow.bIsDynamicOnly;
#else
	return false;
#endif
}

/** True for one face of UE 5.9's per-face cube shadow (the WebGPU fallback), which is not a spot light's 2D shadow. */
bool IsCubeFaceShadow(const FProjectedShadowInfo& Shadow)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 9, 0)
	return Shadow.CubeFaceShadowIndex >= 0;
#else
	return false;
#endif
}

bool IsWholeSceneLocalShadowMap(const FProjectedShadowInfo* Shadow, const FViewInfo& View)
{
	return Shadow && Shadow->bWholeSceneShadow && !Shadow->bDirectionalLight && !Shadow->bRayTracedDistanceField && !IsCubeFaceShadow(*Shadow) &&
		!Shadow->HasVirtualShadowMap() && Shadow->RenderTargets.DepthTarget &&
		(Shadow->DependentView == nullptr || Shadow->DependentView == &View);
}

/** The VSM id of the directional light's clipmap for View, or INDEX_NONE. */
int32 FindDirectionalClipmapId(const FVisibleLightInfo& Visible, const FViewInfo& View)
{
	// A clipmap is built for one view (its dependent view). A secondary stereo view has none of its own and
	// shares the primary view's, which comes first.
	const auto& Clipmaps = Visible.VirtualShadowMapClipmaps;
	const TSharedPtr<FVirtualShadowMapClipmap>* Match = Clipmaps.FindByPredicate(
		[&View](const TSharedPtr<FVirtualShadowMapClipmap>& Clipmap) { return Clipmap.IsValid() && Clipmap->GetDependentView() == &View; });
	if (!Match && !IStereoRendering::IsAPrimaryView(View) && !Clipmaps.IsEmpty() && Clipmaps[0].IsValid())
	{
		Match = &Clipmaps[0];
	}
	return Match ? (*Match)->GetVirtualShadowMapId() : INDEX_NONE;
}

bool IsDirectionalCascade(const FProjectedShadowInfo* Shadow, const FViewInfo& View)
{
	return Shadow && Shadow->bWholeSceneShadow && Shadow->bDirectionalLight && !Shadow->bRayTracedDistanceField &&
		!Shadow->HasVirtualShadowMap() && Shadow->DependentView == &View && Shadow->RenderTargets.DepthTarget;
}
}

bool ShouldProcessView(const FSceneView& View)
{
	return CVarEnable.GetValueOnAnyThread() != 0 && View.bIsViewInfo && View.Family &&
		View.Family->EngineShowFlags.PostProcessing && !View.Family->EngineShowFlags.PathTracing &&
		View.GetFeatureLevel() >= ERHIFeatureLevel::SM5 && !IsMobilePlatform(View.GetShaderPlatform()) &&
		View.UnconstrainedViewRect.Width() > 0 && View.UnconstrainedViewRect.Height() > 0;
}

FRDGTextureRef CreateUnshadowedMask(FRDGBuilder& GraphBuilder, const FViewInfo& View)
{
	FRDGTextureRef Mask = CreateMaskTexture(GraphBuilder, View.UnconstrainedViewRect.Size());
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Mask), FLinearColor::White);
	return Mask;
}

FRDGTextureRef AddMaskPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FMaskInputs& Inputs)
{
	// PostOpaque runs inside FDeferredShadingSceneRenderer::Render, which set itself as the family's renderer.
	if (!View.Family || !Inputs.SceneDepth || !Inputs.SceneTextures)
	{
		return nullptr;
	}
	FSceneRenderer* Renderer = static_cast<FSceneRenderer*>(static_cast<FSceneRendererBase*>(View.Family->GetSceneRenderer()));
	const FViewInfo* FirstView = Renderer->Views.GetData();
	if (!Renderer->Scene || Renderer->Views.IsEmpty() || &View < FirstView || &View >= FirstView + Renderer->Views.Num())
	{
		return nullptr;
	}
	const int32 ViewIndex = int32(&View - FirstView);
	const FScene& Scene = *Renderer->Scene;
	const bool bLog = CVarLog.GetValueOnRenderThread() != 0;

	const TRDGUniformBufferRef<FVirtualShadowMapUniformParameters> VirtualUBRef =
		Renderer->VirtualShadowMapArray.IsAllocated() ? Renderer->VirtualShadowMapArray.GetUniformBuffer(ViewIndex) : nullptr;
	FRDGUniformBuffer* VirtualUB = VirtualUBRef ? static_cast<FRDGUniformBuffer*>(VirtualUBRef) : nullptr;

	// Gather the shadows UE rendered for this view.
	const FLightSceneInfo* MainLight = Scene.SimpleDirectionalLight;
	TArray<const FProjectedShadowInfo*, TInlineAllocator<MaxCascades>> Cascades;
	int32 DirectionalVsmId = INDEX_NONE;
	// Local lights that reach this view. VSM-shadowed and unshadowed ones share one loop (the array); lights with
	// a conventional shadow map get a pass each. Unshadowed lights still count: they fill in the other lights' shadows.
	TArray<FArrayLight, TInlineAllocator<8>> VirtualLights;
	TArray<FArrayLight, TInlineAllocator<8>> UnshadowedLights;
	TArray<FLocalShadowMap, TInlineAllocator<8>> LocalMaps;
	for (auto It = Scene.Lights.CreateConstIterator(); It; ++It)
	{
		const FLightSceneInfo* Light = It->LightSceneInfo;
		if (!Light || !Light->Proxy || !Renderer->VisibleLightInfos.IsValidIndex(It.GetIndex()))
		{
			continue;
		}
		const FVisibleLightInfo& Visible = Renderer->VisibleLightInfos[It.GetIndex()];
		if (Light->Proxy->GetLightType() == LightType_Directional)
		{
			if (Light != MainLight)
			{
				continue;
			}
			if (VirtualUB)
			{
				DirectionalVsmId = FindDirectionalClipmapId(Visible, View);
			}
			if (DirectionalVsmId == INDEX_NONE)
			{
				for (const FProjectedShadowInfo* Shadow : Visible.ShadowsToProject)
				{
					if (IsDirectionalCascade(Shadow, View) && Cascades.Num() < MaxCascades)
					{
						Cascades.Add(Shadow);
					}
				}
			}
			continue;
		}

		if (!View.VisibleLightInfos.IsValidIndex(It.GetIndex()) ||
			!View.VisibleLightInfos[It.GetIndex()].bInViewFrustum || !View.VisibleLightInfos[It.GetIndex()].bInDrawRange)
		{
			continue;
		}
		int32 LocalVsmId = INDEX_NONE;
		if (VirtualUB)
		{
			for (const FProjectedShadowInfo* Shadow : Visible.AllProjectedShadows)
			{
				if (Shadow && Shadow->VirtualShadowMapPerLightCacheEntry.IsValid() &&
					Shadow->VirtualShadowMapPerLightCacheEntry->GetVirtualShadowMapId() >= 0)
				{
					LocalVsmId = Shadow->VirtualShadowMapPerLightCacheEntry->GetVirtualShadowMapId();
					break;
				}
			}
		}
		if (LocalVsmId != INDEX_NONE)
		{
			VirtualLights.Add({ Light, LocalVsmId });
			continue;
		}
		const FProjectedShadowInfo* const* ShadowMap = Visible.ShadowsToProject.FindByPredicate(
			[&View](const FProjectedShadowInfo* Shadow) { return IsWholeSceneLocalShadowMap(Shadow, View); });
		if (ShadowMap)
		{
			LocalMaps.Add({ Light, *ShadowMap });
		}
		else
		{
			UnshadowedLights.Add({ Light, INDEX_NONE });
		}
	}
	// Shadowed lights first, so an overflow drops only fill lights.
	TArray<FArrayLight, TInlineAllocator<8>> ArrayLights = VirtualLights;
	ArrayLights.Append(UnshadowedLights);
	const int32 NumSkippedLights = FMath::Max(0, ArrayLights.Num() - MaxArrayLights);
	ArrayLights.SetNum(FMath::Min(ArrayLights.Num(), MaxArrayLights));

	// Cascades are sampled from one atlas; any cascade packed into another atlas is left out.
	// A cascade split into static and dynamic halves projects both (UE multiplies them); the dynamic half follows the static one.
	Cascades.Sort([](const FProjectedShadowInfo& A, const FProjectedShadowInfo& B)
	{
		if (A.CascadeSettings.ShadowSplitIndex != B.CascadeSettings.ShadowSplitIndex)
		{
			return A.CascadeSettings.ShadowSplitIndex < B.CascadeSettings.ShadowSplitIndex;
		}
		return !IsDynamicOnlyCascade(A) && IsDynamicOnlyCascade(B);
	});
	const int32 NumAllCascades = Cascades.Num();
	if (!Cascades.IsEmpty())
	{
		const IPooledRenderTarget* Atlas = Cascades[0]->RenderTargets.DepthTarget;
		Cascades.RemoveAll([Atlas](const FProjectedShadowInfo* Shadow) { return Shadow->RenderTargets.DepthTarget != Atlas; });
	}
	if (NumSkippedLights > 0 || Cascades.Num() != NumAllCascades)
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogPPShadowTexture, Warning, TEXT("Ignoring %d local lights over %d and %d cascades outside the first CSM atlas."),
				NumSkippedLights, MaxArrayLights, NumAllCascades - Cascades.Num());
		}
	}

	const FIntPoint Extent = View.UnconstrainedViewRect.Size();
	const bool bUsesVirtualShadowMaps = VirtualUB && (!VirtualLights.IsEmpty() || (MainLight && DirectionalVsmId != INDEX_NONE));
	const FVirtualShadowMapBinding VirtualShadowMap = MakeVirtualShadowMapBinding(GraphBuilder, bUsesVirtualShadowMaps ? VirtualUB : nullptr);

	// 1. Local lights accumulate E and Lost in R32F textures (typed UAV loads of R32F are always supported).
	const FRDGTextureDesc AccumDesc = FRDGTextureDesc::Create2D(Extent, PF_R32_FLOAT, FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef LocalEnergy = GraphBuilder.CreateTexture(AccumDesc, TEXT("PPShadowTexture.LocalEnergy"));
	FRDGTextureRef LocalLost = GraphBuilder.CreateTexture(AccumDesc, TEXT("PPShadowTexture.LocalLost"));
	if (!ArrayLights.IsEmpty())
	{
		FPPShadowMaskPassParameters* PassParameters = AllocatePassParameters(GraphBuilder, View, Inputs, Extent);
		bool bAnyVirtual = false;
		for (int32 Index = 0; Index < ArrayLights.Num(); ++Index)
		{
			const int32 VirtualShadowMapId = VirtualUB ? ArrayLights[Index].VirtualShadowMapId : INDEX_NONE;
			bAnyVirtual |= VirtualShadowMapId != INDEX_NONE;
			SetLocalLight(PassParameters->Shader, Index, View, *ArrayLights[Index].Light, VirtualShadowMapId);
		}
		PassParameters->Shader.PPST_NumLights = ArrayLights.Num();
		PassParameters->Shader.PPST_LocalEnergyOutput = GraphBuilder.CreateUAV(LocalEnergy);
		PassParameters->Shader.PPST_LocalLostOutput = GraphBuilder.CreateUAV(LocalLost);
		AddMaskPass(GraphBuilder, View, bAnyVirtual ? EMode::LocalArrayVSM : EMode::LocalArray, PassParameters, VirtualShadowMap);
	}
	else
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(LocalEnergy), 0.0f);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(LocalLost), 0.0f);
	}

	const float SpotTransitionScale = GetRenderThreadFloat(TEXT("r.Shadow.SpotLightTransitionScale"), 60.0f);
	const float SpotReceiverBias = GetReceiverBias(TEXT("r.Shadow.SpotLightReceiverBias"));
	int32 NumSpot = 0, NumCube = 0;
	for (const FLocalShadowMap& Local : LocalMaps)
	{
		const FProjectedShadowInfo& Shadow = *Local.Shadow;
		FPPShadowMaskPassParameters* PassParameters = AllocatePassParameters(GraphBuilder, View, Inputs, Extent);
		FPPShadowMaskCS::FParameters& P = PassParameters->Shader;
		SetLocalLight(P, 0, View, *Local.Light, INDEX_NONE);
		P.PPST_NumLights = 1;
		P.PPST_ShadowSharpen = Local.Light->Proxy->GetShadowSharpen() * 7.0f + 1.0f;
		P.PPST_LocalEnergyOutput = GraphBuilder.CreateUAV(LocalEnergy);
		P.PPST_LocalLostOutput = GraphBuilder.CreateUAV(LocalLost);
		EMode Mode;
		if (Shadow.bOnePassPointLightShadow)
		{
			// Point and rect lights: one-pass cube map (see FOnePassPointShadowProjectionShaderParameters).
			P.PPST_CubeShadowDepth = GraphBuilder.RegisterExternalTexture(Shadow.RenderTargets.DepthTarget);
			for (int32 Face = 0; Face < 6; ++Face)
			{
				P.PPST_CubeViewProjection[Face] = Shadow.OnePassShadowViewProjectionMatrices.IsValidIndex(Face) ?
					FMatrix44f(Shadow.OnePassShadowViewProjectionMatrices[Face]) : FMatrix44f(EForceInit::ForceInitToZero);
			}
			P.PPST_CubeParams = FVector4f(1.0f / Shadow.ResolutionX, Shadow.GetShaderDepthBias(), Shadow.GetShaderSlopeDepthBias(),
				Shadow.GetShaderMaxSlopeDepthBias());
			P.InvShadowmapResolution = P.PPST_CubeParams.X;
			P.PPST_CubeFade = Shadow.FadeAlphas.IsValidIndex(ViewIndex) ? Shadow.FadeAlphas[ViewIndex] : 1.0f;
			Mode = EMode::LocalCube;
			++NumCube;
		}
		else
		{
			// Spot lights: one tile of a 2D atlas. Transition size as ComputeTransitionSize for this case.
			P.PPST_ShadowDepth = GraphBuilder.RegisterExternalTexture(Shadow.RenderTargets.DepthTarget);
			SetShadowMapSlot(P, 0, View, ViewIndex, Shadow, UE_BIG_NUMBER,
				1.0f / SpotTransitionScale * 2.0f * Local.Light->Proxy->GetUserShadowBias(), SpotReceiverBias);
			P.PPST_NumShadows = 1;
			Mode = EMode::LocalSpot;
			++NumSpot;
		}
		AddMaskPass(GraphBuilder, View, Mode, PassParameters, FVirtualShadowMapBinding());
	}

	// 2. Compose: R = main directional light, G = 1 - Lost / (sun irradiance * R + E).
	FRDGTextureRef Mask = CreateMaskTexture(GraphBuilder, Extent);
	FPPShadowMaskPassParameters* PassParameters = AllocatePassParameters(GraphBuilder, View, Inputs, Extent);
	FPPShadowMaskCS::FParameters& P = PassParameters->Shader;
	P.PPST_LocalEnergy = LocalEnergy;
	P.PPST_LocalLost = LocalLost;
	P.PPST_MaskOutput = GraphBuilder.CreateUAV(Mask);
	EMode ComposeMode = EMode::ComposeNone;
	if (MainLight)
	{
		FLightRenderParameters SunParameters{};
		MainLight->Proxy->GetLightShaderParameters(SunParameters);
		P.PPST_DirectionalEnergy = GetDiffuseLuminance(SunParameters);
		P.PPST_DirectionalToLight = FVector3f(-MainLight->Proxy->GetDirection());
	}
	if (MainLight && DirectionalVsmId != INDEX_NONE && VirtualUB)
	{
		P.PPST_DirectionalVsmId = DirectionalVsmId;
		ComposeMode = EMode::ComposeVSM;
	}
	else if (MainLight && !Cascades.IsEmpty())
	{
		const float CSMDepthBias = GetRenderThreadFloat(TEXT("r.Shadow.CSMDepthBias"), 10.0f);
		const float CSMReceiverBias = GetReceiverBias(TEXT("r.Shadow.CSMReceiverBias"));
		for (int32 Index = 0; Index < Cascades.Num(); ++Index)
		{
			const FProjectedShadowInfo& Shadow = *Cascades[Index];
			// ComputeTransitionSize for whole scene directional shadows.
			const float TransitionSize = CSMDepthBias / (Shadow.MaxSubjectZ - Shadow.MinSubjectZ) *
				(Shadow.ShadowBounds.W / Shadow.ResolutionX) * MainLight->Proxy->GetUserShadowBias();
			SetShadowMapSlot(P, Index, View, ViewIndex, Shadow, Shadow.CascadeSettings.SplitFar, TransitionSize, CSMReceiverBias);
			if (Index > 0 && IsDynamicOnlyCascade(Shadow) &&
				Cascades[Index - 1]->CascadeSettings.ShadowSplitIndex == Shadow.CascadeSettings.ShadowSplitIndex)
			{
				P.PPST_CascadeContinuationMask |= 1u << Index;
			}
		}
		P.PPST_ShadowDepth = GraphBuilder.RegisterExternalTexture(Cascades[0]->RenderTargets.DepthTarget);
		P.PPST_NumShadows = Cascades.Num();
		P.PPST_ShadowSharpen = MainLight->Proxy->GetShadowSharpen() * 7.0f + 1.0f;
		ComposeMode = EMode::ComposeCSM;
	}
	AddMaskPass(GraphBuilder, View, ComposeMode, PassParameters, VirtualShadowMap);

	if (bLog)
	{
		UE_LOG(LogPPShadowTexture, Display, TEXT("PPShadowTexture view %d %dx%d: main=%s cascades=%d vsm_id=%d local vsm=%d unshadowed=%d spot=%d cube=%d sun=%s sun_lux=%.2f"),
			ViewIndex, Extent.X, Extent.Y, GetModeName(ComposeMode), Cascades.Num(), DirectionalVsmId,
			VirtualLights.Num(), UnshadowedLights.Num(), NumSpot, NumCube,
			MainLight ? *MainLight->Proxy->GetOwnerNameOrLabel() : TEXT("none"), P.PPST_DirectionalEnergy);
	}
	return Mask;
}

void RegisterUserSceneTexture(const FViewInfo& View, FRDGTextureRef Mask)
{
	// The engine's allocation helper (FindOrAddUserSceneTexture, UE 5.8 / 5.9) is not exported and would inherit scene color's
	// format, so the existing texture is inserted directly as the most recent entry for this name.
	const FSceneTextures& SceneTextures = View.GetSceneTextures();
	TArray<FTransientUserSceneTexture>& Entries = SceneTextures.UserSceneTextures.FindOrAdd(TextureName);
	const int32 ViewIndex = View.Family->Views.IndexOfByKey(&View);
	FTransientUserSceneTexture Entry{};
	Entry.Texture = Mask;
	Entry.ResolutionDivisor = FIntPoint(1, 1);
	Entry.AllocationOrder = static_cast<uint16>(Entries.Num());
	// bUsed only feeds r.UserSceneTextureDebug's "unused output" warning; the texture is optional for materials.
	Entry.bUsed = true;
	Entry.ViewMask = ViewIndex >= 0 && ViewIndex < 32 ? (1u << ViewIndex) : 0u;
	Entries.Insert(Entry, 0);
#if !UE_BUILD_SHIPPING
	SceneTextures.UserSceneTextureEvents.Add({ EUserSceneTextureEvent::Output, TextureName, Entry.AllocationOrder,
		static_cast<uint16>(FMath::Max(0, ViewIndex)), nullptr, Mask->Desc.Extent });
#endif
}
}

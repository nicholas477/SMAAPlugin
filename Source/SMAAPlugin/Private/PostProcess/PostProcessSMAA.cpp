#include "PostProcess/PostProcessSMAA.h"

#include "PostProcess/PostProcessing.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "Rendering/Texture2DResource.h"
#include "ScenePrivate.h"
#include "SMAASceneExtension.h"
#include "SceneViewExtension.h"

#include "SceneView.h"
#include "ScreenPass.h"
#include "CommonRenderResources.h"
#include "Engine/TextureRenderTarget2D.h"
#include "DynamicResolutionState.h"
#include "FXRenderingUtils.h"

UE_DISABLE_OPTIMIZATION
DECLARE_GPU_STAT(SMAAPass)
DECLARE_GPU_STAT_NAMED(SMAADispatch, TEXT("SMAA Dispatch"));

TAutoConsoleVariable<int32> CVarSMAAQuality(
	TEXT("r.SMAA.Quality"), 3,
	TEXT("Selects the quality permutation of SMAA.\n")
		TEXT(" 0: Low Preset \n")
			TEXT(" 1: Medium Preset \n")
				TEXT(" 2: High Preset \n")
					TEXT(" 3: Ultra Preset (Default) \n"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSMAAEdgeMode(
	TEXT("r.SMAA.EdgeDetector"), 3,
	TEXT("Data used by SMAA's Edge Detector\n")
		TEXT(" 0 - Depth\n")
			TEXT(" 1 - Luminance\n")
				TEXT(" 2 - Colour\n")
					TEXT(" 3 - World Normal (Default)\n"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSMAAPredicationSource(
	TEXT("r.SMAA.Predicate"), 0,
	TEXT("Predication Source\n")
		TEXT(" 0 - None (Default)\n")
			TEXT(" 1 - Depth\n")
				TEXT(" 2 - World Normal\n")
					TEXT(" 3 - Spec, Rough, Metal\n"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSMAAMaxSearchSteps(TEXT("r.SMAA.MaxSearchSteps"), 8,
	TEXT("Maximum steps performed in Horizontal/Vert patterns [0 - 112]"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSMAAMaxDiagonalSearchSteps(TEXT("r.SMAA.MaxSearchStepsDiagonal"), 16,
	TEXT("Maximum steps performed in Diagonal patterns [0 - 20]"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSMAACornerRounding(TEXT("r.SMAA.CornerRounding"), 25,
	TEXT("Specifies how much sharp corners will be rounded [0 - 100]"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSMAAAdaptationFactor(TEXT("r.SMAA.AdaptationFactor"), 2.0,
	TEXT("Controls Adaptation Factor for edge discard\n")
		TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSMAAReprojectionWeight(TEXT("r.SMAA.ReprojectionWeight"), 25,
	TEXT("Controls Reprojection [0 - 100]\n")
		TEXT("Using low values will exhibit ghosting, while using high values will disable temporal supersampling under motion"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSMAAPredicationThreshold(TEXT("r.SMAA.PredicationThreshold"), 0.04,
	TEXT("Controls Reprojection [0 - 1] (Default 0.04) \n")
		TEXT("Threshold to be used in the additional predication buffer."),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSMAAPredicationScale(TEXT("r.SMAA.PredicationScale"), 2.0,
	TEXT("Controls Reprojection [1 - 5] (Default 2.0) \n")
		TEXT("How much to scale the global threshold used for luma or color edge detection when using predication"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSMAAPredicationStrength(TEXT("r.SMAA.PredicationStrength"), 0.4,
	TEXT("Controls Reprojection [0 - 1] (Default 0.4) \n")
		TEXT("How much to locally decrease the threshold."),
	ECVF_Scalability | ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSMAATemporalHistoryBias(TEXT("r.SMAA.TemporalHistoryBias"), 0.4,
	TEXT("Controls base weight from prior frames [0 - 1) (Default 0.4)"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

///// ///// ////////// ///// /////
// SMAA Shaders
//

/**
 * SMAA Edge Detection
 */
class FSMAAEdgeDetectionCS : public FGlobalShader
{
public:
	static const int ThreadgroupSizeX = 8;
	static const int ThreadgroupSizeY = 8;
	static const int ThreadgroupSizeZ = 1;

	DECLARE_GLOBAL_SHADER(FSMAAEdgeDetectionCS);
	SHADER_USE_PARAMETER_STRUCT(FSMAAEdgeDetectionCS, FGlobalShader);

	class FSMAAPresetConfigDim : SHADER_PERMUTATION_ENUM_CLASS("SMAA_PRESET", ESMAAPreset);
	class FSMAAEdgeModeConfigDim : SHADER_PERMUTATION_ENUM_CLASS("SMAA_EDMODE", ESMAAEdgeDetectors);
	class FSMAAPredicateConfigDim : SHADER_PERMUTATION_BOOL("SMAA_PREDICATION");

	using FPermutationDomain =
		TShaderPermutationDomain<FSMAAPresetConfigDim, FSMAAEdgeModeConfigDim, FSMAAPredicateConfigDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	RDG_TEXTURE_ACCESS(DepthTexture, ERHIAccess::SRVCompute)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputDepth)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputSceneColor)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, Predicate)
	SHADER_PARAMETER(float, AdaptationFactor)
	SHADER_PARAMETER(float, PredicationThreshold)
	SHADER_PARAMETER(float, PredicationScale)
	SHADER_PARAMETER(float, PredicationStrength)
	SHADER_PARAMETER_SAMPLER(SamplerState, PointTextureSampler)
	SHADER_PARAMETER_SAMPLER(SamplerState, BilinearTextureSampler)
	SHADER_PARAMETER(FVector4f, ViewportMetrics)
	SHADER_PARAMETER(float, NormalisedCornerRounding)
	SHADER_PARAMETER(float, MaxDiagonalSearchSteps)
	SHADER_PARAMETER(float, MaxSearchSteps)
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, EdgesTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;

		//TODO: Kory
		//FPermutationDomain PermutationVector(Parameters.PermutationId);
		//if (PermutationVector.Get<FSMAAEdgeModeConfigDim>() == ESMAAEdgeDetectors::Luminance)
		//{
		//	return true;
		//}
		//return false;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), ThreadgroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), ThreadgroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEZ"), ThreadgroupSizeZ);
		OutEnvironment.SetDefine(TEXT("COMPUTE_SHADER"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FSMAAEdgeDetectionCS, "/SMAAPlugin/Private/SMAA_EdgeDetection.usf", "EdgeDetectionCS",
	SF_Compute);

/**
 * SMAA Blending Weight Calculation
 */
class FSMAABlendingWeightsCS : public FGlobalShader
{
public:
	static const int ThreadgroupSizeX = 8;
	static const int ThreadgroupSizeY = 8;
	static const int ThreadgroupSizeZ = 1;

	DECLARE_GLOBAL_SHADER(FSMAABlendingWeightsCS);
	SHADER_USE_PARAMETER_STRUCT(FSMAABlendingWeightsCS, FGlobalShader);

	class FSMAAPresetConfigDim : SHADER_PERMUTATION_ENUM_CLASS("SMAA_PRESET", ESMAAPreset);

	using FPermutationDomain = TShaderPermutationDomain<FSMAAPresetConfigDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	RDG_TEXTURE_ACCESS(DepthTexture, ERHIAccess::SRVCompute)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputEdges)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, AreaTexture)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SearchTexture)
	SHADER_PARAMETER(FVector2f, TemporalJitterPixels)
	// SHADER_PARAMETER_SRV(Texture2D, AreaTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, PointTextureSampler)
	SHADER_PARAMETER_SAMPLER(SamplerState, BilinearTextureSampler)
	SHADER_PARAMETER(FVector4f, ViewportMetrics)
	SHADER_PARAMETER(FVector4f, SubpixelWeights)
	SHADER_PARAMETER(float, NormalisedCornerRounding)
	SHADER_PARAMETER(float, MaxDiagonalSearchSteps)
	SHADER_PARAMETER(float, MaxSearchSteps)
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, BlendTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), ThreadgroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), ThreadgroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEZ"), ThreadgroupSizeZ);
		OutEnvironment.SetDefine(TEXT("COMPUTE_SHADER"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FSMAABlendingWeightsCS, "/SMAAPlugin/Private/SMAA_BlendWeighting.usf",
	"BlendWeightingCS", SF_Compute);

/**
 * SMAA Neighbour Blending
 */
class FSMAANeighbourhoodBlendingCS : public FGlobalShader
{
public:
	static const int ThreadgroupSizeX = 8;
	static const int ThreadgroupSizeY = 8;
	static const int ThreadgroupSizeZ = 1;

	DECLARE_GLOBAL_SHADER(FSMAANeighbourhoodBlendingCS);
	SHADER_USE_PARAMETER_STRUCT(FSMAANeighbourhoodBlendingCS, FGlobalShader);

	class FSMAAPresetConfigDim : SHADER_PERMUTATION_ENUM_CLASS("SMAA_PRESET", ESMAAPreset);
	class FSMAAReprojectionDim : SHADER_PERMUTATION_BOOL("SMAA_REPROJECTION");

	using FPermutationDomain =
		TShaderPermutationDomain<FSMAAPresetConfigDim, FSMAAReprojectionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	RDG_TEXTURE_ACCESS(DepthTexture, ERHIAccess::SRVCompute)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneColour)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, VelocityTexture)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputBlend)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneDepth)
	SHADER_PARAMETER_SAMPLER(SamplerState, PointTextureSampler)
	SHADER_PARAMETER_SAMPLER(SamplerState, BilinearTextureSampler)
	SHADER_PARAMETER(FVector4f, ViewportMetrics)
	SHADER_PARAMETER(float, NormalisedCornerRounding)
	SHADER_PARAMETER(float, MaxDiagonalSearchSteps)
	SHADER_PARAMETER(float, MaxSearchSteps)
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, FinalFrame)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), ThreadgroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), ThreadgroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEZ"), ThreadgroupSizeZ);
		OutEnvironment.SetDefine(TEXT("COMPUTE_SHADER"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FSMAANeighbourhoodBlendingCS, "/SMAAPlugin/Private/SMAA_NeighbourhoodBlend.usf",
	"NeighbourhoodBlendingCS", SF_Compute);

/**
 * SMAA Blending Weight Calculation
 */
class FSMAATemporalResolveCS : public FGlobalShader
{
public:
	static const int ThreadgroupSizeX = 8;
	static const int ThreadgroupSizeY = 8;
	static const int ThreadgroupSizeZ = 1;

	DECLARE_GLOBAL_SHADER(FSMAATemporalResolveCS);
	SHADER_USE_PARAMETER_STRUCT(FSMAATemporalResolveCS, FGlobalShader);

	class FSMAAPresetConfigDim : SHADER_PERMUTATION_ENUM_CLASS("SMAA_PRESET", ESMAAPreset);
	class FSMAAReprojectionDim : SHADER_PERMUTATION_BOOL("SMAA_REPROJECTION");

	using FPermutationDomain =
		TShaderPermutationDomain<FSMAAPresetConfigDim, FSMAAReprojectionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	RDG_TEXTURE_ACCESS(DepthTexture, ERHIAccess::SRVCompute)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, CurrentSceneColour)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, PastSceneColour)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneDepth)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, VelocityTexture)

	SHADER_PARAMETER_SAMPLER(SamplerState, PointTextureSampler)
	SHADER_PARAMETER_SAMPLER(SamplerState, BilinearTextureSampler)
	SHADER_PARAMETER(FVector4f, ViewportMetrics)
	SHADER_PARAMETER(FVector4f, LimitedViewportSize)
	SHADER_PARAMETER(float, NormalisedCornerRounding)
	SHADER_PARAMETER(float, MaxDiagonalSearchSteps)
	SHADER_PARAMETER(float, MaxSearchSteps)
	SHADER_PARAMETER(float, ReprojectionWeight)
	SHADER_PARAMETER(float, TemporalHistoryBias)
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, Resolved)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), ThreadgroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), ThreadgroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEZ"), ThreadgroupSizeZ);
		OutEnvironment.SetDefine(TEXT("COMPUTE_SHADER"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSMAATemporalResolveCS, "/SMAAPlugin/Private/SMAA_T2XResolve.usf", "TemporalResolveCS", SF_Compute);

ESMAAPreset GetSMAAPreset()
{
	return ESMAAPreset(FMath::Clamp(CVarSMAAQuality.GetValueOnRenderThread(), 0, 3));
}

ESMAAEdgeDetectors GetSMAAEdgeDetectors()
{
	return ESMAAEdgeDetectors(FMath::Clamp(CVarSMAAEdgeMode.GetValueOnRenderThread(), 0, 3));
}

ESMAAPredicationTexture GetPredicateSource()
{
	return ESMAAPredicationTexture(FMath::Clamp(CVarSMAAPredicationSource.GetValueOnRenderThread(), 0, 3));
}

uint8 GetSMAAMaxSearchSteps()
{
	return FMath::Clamp(CVarSMAAMaxSearchSteps.GetValueOnRenderThread(), 0, 112);
}

uint8 GetSMAAMaxDiagonalSearchSteps()
{
	return FMath::Clamp(CVarSMAAMaxDiagonalSearchSteps.GetValueOnRenderThread(), 0, 20);
}

uint8 GetSMAACornerRounding()
{
	return FMath::Clamp(CVarSMAACornerRounding.GetValueOnRenderThread(), 0, 100);
}

float GetSMAAAdaptationFactor()
{
	return FMath::Clamp(CVarSMAAAdaptationFactor.GetValueOnRenderThread(), 0.f, 10.f);
}

float GetSMAAReprojectionWeight()
{
	return FMath::Clamp(CVarSMAAReprojectionWeight.GetValueOnRenderThread(), 0.f, 100.f);
}

float GetSMAAPredicationThreshold()
{
	return FMath::Clamp(CVarSMAAPredicationThreshold.GetValueOnRenderThread(), 0.f, 1.f);
}

float GetSMAAPredicationScale()
{
	return FMath::Clamp(CVarSMAAPredicationScale.GetValueOnRenderThread(), 1.f, 5.f);
}

float GetSMAAPredicationStrength()
{
	return FMath::Clamp(CVarSMAAPredicationStrength.GetValueOnRenderThread(), 0.f, 1.f);
}

float GetSMAATemporalHistoryBias()
{
	return FMath::Clamp(CVarSMAATemporalHistoryBias.GetValueOnRenderThread(), 0.f, (1.f - SMALL_NUMBER));
}

//// FlipNames
//TCHAR* FlipNames[2] = {
//	TEXT("SMAA0"),
//	TEXT("SMAA1")
//};

FVector4f SubpixelJitterWeights[2] = {
	FVector4f(1, 1, 1, 0),
	FVector4f(2, 2, 2, 0)
};

FScreenPassTexture AddSMAAPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FSMAAInputs& Inputs, const FPostProcessMaterialInputs& InOutInputs, TSharedRef<struct FSMAAViewData> ViewData)
{
	check(Inputs.SceneColor.IsValid());
	check(Inputs.Quality != ESMAAPreset::MAX);
	check(Inputs.EdgeMode != ESMAAEdgeDetectors::MAX);
	RDG_EVENT_SCOPE(GraphBuilder, "SMAA T2x");

	FIntPoint InputExtents = Inputs.SceneColor.Texture->Desc.Extent; // View.ViewRect.Size();
	FIntRect InputRect = View.ViewRect;
	InputRect.Min = FIntPoint(0, 0);
	InputRect.Min = InputExtents;
	FIntPoint BackingSize = InputExtents;
	QuantizeSceneBufferSize(InputExtents, BackingSize);

	FIntRect OutputRect = View.ViewRect;
	FIntPoint OutputExtents = View.ViewRect.Size();

	FScreenPassTexture Output = Inputs.OverrideOutput;

	if (!Output.IsValid())
	{
		FRDGTextureDesc WriteOutTextureDesc =
			FRDGTextureDesc::Create2D(BackingSize, PF_FloatRGBA, FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);

		Output = FScreenPassTexture(
			GraphBuilder.CreateTexture(WriteOutTextureDesc, TEXT("SMAA.Output")),
			OutputRect);
	}

	FRHISamplerState* BilinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	FRHISamplerState* PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	auto RTMetrics = FVector4f(1.0 / BackingSize.X, 1.0 / BackingSize.Y, BackingSize.X, BackingSize.Y);

	const FTexture2DResource* AreaResource = ViewData->SMAAAreaTexture;
	if (!AreaResource)
	{
		// Bail
		return Inputs.SceneColor;
	}

	const FTexture2DResource* SearchResource = ViewData->SMAASearchTexture;
	if (!SearchResource)
	{
		// Bail
		return Inputs.SceneColor;
	}

	FRHITexture* AreaTextureRHI = AreaResource->GetTexture2DRHI();
	if (!AreaTextureRHI)
	{
		// Bail
		return Inputs.SceneColor;
	}
	FRDGTextureRef AreaTexture = RegisterExternalTexture(GraphBuilder, AreaTextureRHI, TEXT("SMAA.AreaTexture"));

	FRHITexture* SearchTextureRHI = SearchResource->GetTexture2DRHI();
	if (!SearchTextureRHI)
	{
		// Bail
		return Inputs.SceneColor;
	}
	FRDGTextureRef SearchTexture = RegisterExternalTexture(GraphBuilder, SearchTextureRHI, TEXT("SMAA.SearchTexture"));

	// Create Textures for SMAA
	FRDGTextureDesc EdgesTextureDesc =
		FRDGTextureDesc::Create2D(BackingSize, PF_FloatRGBA, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);

	FRDGTextureRef EdgesTexture = GraphBuilder.CreateTexture(EdgesTextureDesc, TEXT("SMAA.EdgesTexture"));

	// Blend Texture
	FRDGTextureDesc BlendTextureDesc =
		FRDGTextureDesc::Create2D(BackingSize, PF_FloatRGBA, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);

	FRDGTextureRef BlendTexture = GraphBuilder.CreateTexture(BlendTextureDesc, TEXT("SMAA.BlendTexture"));

	// Modification!
	// Fall back to SMAA 1x?
	bool bCameraCut = false;
	FRDGTextureRef LastRGBA = GSystemTextures.GetBlackDummy(GraphBuilder);

	//if (View.PrevViewInfo.SMAAHistory.IsValid())
	if (ViewData->SMAAHistory.IsValid())
	{
		//LastRGBA = GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.SMAAHistory.PastFrame);
		LastRGBA = GraphBuilder.RegisterExternalTexture(ViewData->SMAAHistory.PastFrame);
		bCameraCut = View.bCameraCut;
	}

	//InOutInputs.SceneTextures.SceneTextures->GetContents()->SceneColorTexture;
	FRDGTextureRef SceneColor = Inputs.SceneColor.Texture;
	//FRDGTextureRef SceneDepth = Inputs.SceneDepth.Texture;
	FRDGTextureRef Velocity = Inputs.SceneVelocity.Texture;

	// Create Depth SRV Desc
	//FRDGTextureSRVDesc DepthSRVDesc = FRDGTextureSRVDesc::Create(SceneDepth);
	FRDGTextureSRVDesc AreaTextureSRVDesc = FRDGTextureSRVDesc::Create(AreaTexture);
	FRDGTextureSRVDesc SearchTextureSRVDesc = FRDGTextureSRVDesc::Create(SearchTexture);
	FRDGTextureSRVDesc SceneColourSRVDesc = FRDGTextureSRVDesc::Create(SceneColor);
	FRDGTextureSRVDesc PrevSceneColourSRVDesc = FRDGTextureSRVDesc::Create(LastRGBA);
	FRDGTextureSRVDesc EdgesSRVDesc = FRDGTextureSRVDesc::Create(EdgesTexture);
	FRDGTextureSRVDesc BlendSRVDesc = FRDGTextureSRVDesc::Create(BlendTexture);
	FRDGTextureSRVDesc WriteOutTextureSRVDesc = FRDGTextureSRVDesc::Create(Output.Texture);
	FRDGTextureSRVDesc VelocityDesc = FRDGTextureSRVDesc::Create(Velocity);

	FRDGTextureSRVRef AreaTextureSRV = GraphBuilder.CreateSRV(AreaTextureSRVDesc);
	FRDGTextureSRVRef SearchTextureSRV = GraphBuilder.CreateSRV(SearchTextureSRVDesc);
	FRDGTextureSRVRef DepthSRV = GraphBuilder.CreateSRV(InOutInputs.SceneTextures.SceneTextures->GetContents()->SceneDepthTexture);
	FRDGTextureSRVRef ColourSRV = GraphBuilder.CreateSRV(SceneColourSRVDesc);

	TRDGTextureAccess<ERHIAccess::SRVCompute> SceneDepth = InOutInputs.SceneTextures.SceneTextures->GetContents()->SceneDepthTexture;

	// Permutations
	ESMAAPreset Preset = Inputs.Quality;
	ESMAAEdgeDetectors EdgeDetectorMode = Inputs.EdgeMode;
	auto Rounding = Inputs.CornerRounding * 0.01f;
	auto MaxStepDiag = Inputs.MaxDiagonalSearchSteps;
	auto MaxStepOrth = Inputs.MaxSearchSteps;
	auto ProjectionWeight = Inputs.ReprojectionWeight;
	ESMAAPredicationTexture PredicateSource = Inputs.PredicationSource;
	auto AdaptationFactor = Inputs.AdaptationFactor;
	auto PredicationThreshold = Inputs.PredicationThreshold;
	auto PredicationScale = Inputs.PredicationScale;
	auto PredicationStrength = Inputs.PredicationStrength;
	auto TemporalHistoryBias = Inputs.TemporalHistoryBias;

	// Wanted Predicate Texture
	FRDGTextureSRVRef PredicateTexture = GraphBuilder.CreateSRV(GSystemTextures.GetWhiteDummy(GraphBuilder));
	switch (PredicateSource)
	{
		case ESMAAPredicationTexture::Depth:
			PredicateTexture = DepthSRV;
			break;
		case ESMAAPredicationTexture::WorldNormal:
			PredicateTexture = GraphBuilder.CreateSRV(InOutInputs.SceneTextures.SceneTextures->GetContents()->GBufferATexture);
			break;
		case ESMAAPredicationTexture::MRS:
			PredicateTexture = GraphBuilder.CreateSRV(InOutInputs.SceneTextures.SceneTextures->GetContents()->GBufferBTexture);
			//PredicateTexture = GraphBuilder.CreateSRV(Inputs.PredicateTexture.Texture);
			break;
		case ESMAAPredicationTexture::None:;
		case ESMAAPredicationTexture::MAX:;
		default:;
	}

	{
		FSMAAEdgeDetectionCS::FPermutationDomain PermutationVector;

		PermutationVector.Set<FSMAAEdgeDetectionCS::FSMAAPresetConfigDim>(Preset);
		PermutationVector.Set<FSMAAEdgeDetectionCS::FSMAAEdgeModeConfigDim>(EdgeDetectorMode);
		PermutationVector.Set<FSMAAEdgeDetectionCS::FSMAAPredicateConfigDim>(ESMAAPredicationTexture::None != PredicateSource);

		FSMAAEdgeDetectionCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FSMAAEdgeDetectionCS::FParameters>();
		FRDGTextureUAVDesc OutputDesc(EdgesTexture);

		PassParameters->DepthTexture = SceneDepth;
		PassParameters->PointTextureSampler = PointClampSampler;
		PassParameters->BilinearTextureSampler = BilinearClampSampler;

		// Pass colour for Depth, Luma, and Colour
		if (EdgeDetectorMode < ESMAAEdgeDetectors::Normal)
		{
			PassParameters->InputSceneColor = ColourSRV;
		}
		else if (ESMAAEdgeDetectors::Normal == EdgeDetectorMode)
		{
			//PassParameters->InputSceneColor = GraphBuilder.CreateSRV(Inputs.WorldNormal.Texture);
			PassParameters->InputSceneColor = GraphBuilder.CreateSRV(InOutInputs.SceneTextures.SceneTextures->GetContents()->GBufferATexture);
		}

		PassParameters->InputDepth = DepthSRV;
		PassParameters->ViewportMetrics = RTMetrics;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->NormalisedCornerRounding = Rounding;
		PassParameters->MaxSearchSteps = MaxStepOrth;
		PassParameters->MaxDiagonalSearchSteps = MaxStepDiag;
		PassParameters->Predicate = PredicateTexture;
		PassParameters->AdaptationFactor = AdaptationFactor;
		PassParameters->PredicationThreshold = PredicationThreshold;
		PassParameters->PredicationScale = PredicationScale;
		PassParameters->PredicationStrength = PredicationStrength;
		PassParameters->EdgesTexture = GraphBuilder.CreateUAV(OutputDesc);

		TShaderMapRef<FSMAAEdgeDetectionCS> ComputeShaderSMAAED(View.ShaderMap, PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("SMAA/EdgeDetection (CS)"), ComputeShaderSMAAED, PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(EdgesTexture->Desc.Extent.X, EdgesTexture->Desc.Extent.Y, 1),
				FIntVector(FSMAAEdgeDetectionCS::ThreadgroupSizeX,
					FSMAAEdgeDetectionCS::ThreadgroupSizeY,
					FSMAAEdgeDetectionCS::ThreadgroupSizeZ)));
	}

	// Blend
	{
		FSMAABlendingWeightsCS::FPermutationDomain PermutationVector;

		PermutationVector.Set<FSMAABlendingWeightsCS::FSMAAPresetConfigDim>(Preset);

		FSMAABlendingWeightsCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FSMAABlendingWeightsCS::FParameters>();
		FRDGTextureUAVDesc OutputDesc(BlendTexture);

		PassParameters->DepthTexture = SceneDepth;
		PassParameters->PointTextureSampler = TStaticSamplerState<SF_Point>::GetRHI();
		PassParameters->BilinearTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
		PassParameters->AreaTexture = AreaTextureSRV;
		PassParameters->InputEdges = GraphBuilder.CreateSRV(EdgesSRVDesc);
		PassParameters->TemporalJitterPixels = FVector2f(View.TemporalJitterPixels);
		PassParameters->SubpixelWeights = SubpixelJitterWeights[ViewData->JitterIndex & 1];
		PassParameters->SearchTexture = SearchTextureSRV;
		PassParameters->ViewportMetrics = RTMetrics;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->NormalisedCornerRounding = Rounding;
		PassParameters->MaxSearchSteps = MaxStepOrth;
		PassParameters->MaxDiagonalSearchSteps = MaxStepDiag;
		PassParameters->BlendTexture = GraphBuilder.CreateUAV(OutputDesc);

		TShaderMapRef<FSMAABlendingWeightsCS> ComputeShaderSMAABW(View.ShaderMap, PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("SMAA/BlendWeights (CS)"), ComputeShaderSMAABW, PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(EdgesTexture->Desc.Extent.X, EdgesTexture->Desc.Extent.Y, 1),
				FIntVector(FSMAABlendingWeightsCS::ThreadgroupSizeX,
					FSMAABlendingWeightsCS::ThreadgroupSizeY,
					FSMAABlendingWeightsCS::ThreadgroupSizeZ)));
	}

	// Neighbourhood Blending
	{
		FSMAANeighbourhoodBlendingCS::FPermutationDomain PermutationVector;

		PermutationVector.Set<FSMAANeighbourhoodBlendingCS::FSMAAPresetConfigDim>(Preset);
		PermutationVector.Set<FSMAANeighbourhoodBlendingCS::FSMAAReprojectionDim>(true);

		FSMAANeighbourhoodBlendingCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FSMAANeighbourhoodBlendingCS::FParameters>();
		FRDGTextureUAVDesc OutputDesc(EdgesTexture);

		PassParameters->DepthTexture = SceneDepth;
		PassParameters->PointTextureSampler = PointClampSampler;
		PassParameters->BilinearTextureSampler = BilinearClampSampler;
		PassParameters->SceneColour = GraphBuilder.CreateSRV(SceneColourSRVDesc);
		PassParameters->InputBlend = GraphBuilder.CreateSRV(BlendSRVDesc);
		PassParameters->SceneDepth = DepthSRV;
		PassParameters->VelocityTexture = InOutInputs.GetInput(EPostProcessMaterialInput::Velocity).TextureSRV;
		PassParameters->ViewportMetrics = RTMetrics;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->NormalisedCornerRounding = Rounding;
		PassParameters->MaxSearchSteps = MaxStepOrth;
		PassParameters->MaxDiagonalSearchSteps = MaxStepDiag;

		// Write out to Final if bCameraCut
		if (bCameraCut)
		{
			PassParameters->FinalFrame = GraphBuilder.CreateUAV(Output.Texture);
		}
		else
		{
			PassParameters->FinalFrame = GraphBuilder.CreateUAV(OutputDesc);
		}

		TShaderMapRef<FSMAANeighbourhoodBlendingCS> ComputeShaderSMAANB(View.ShaderMap, PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("SMAA/NeighbourhoodBlending (CS)"), ComputeShaderSMAANB, PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(EdgesTexture->Desc.Extent.X, EdgesTexture->Desc.Extent.Y, 1),
				FIntVector(FSMAANeighbourhoodBlendingCS::ThreadgroupSizeX,
					FSMAANeighbourhoodBlendingCS::ThreadgroupSizeY,
					FSMAANeighbourhoodBlendingCS::ThreadgroupSizeZ)));
	}

	// Temporal Resolve
	if (!bCameraCut)
	{
		FSMAATemporalResolveCS::FPermutationDomain PermutationVector;

		PermutationVector.Set<FSMAATemporalResolveCS::FSMAAPresetConfigDim>(Preset);
		PermutationVector.Set<FSMAATemporalResolveCS::FSMAAReprojectionDim>(true);

		FSMAATemporalResolveCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FSMAATemporalResolveCS::FParameters>();

		PassParameters->DepthTexture = SceneDepth;
		PassParameters->PointTextureSampler = PointClampSampler;
		PassParameters->BilinearTextureSampler = BilinearClampSampler;
		PassParameters->CurrentSceneColour = GraphBuilder.CreateSRV(EdgesSRVDesc);
		PassParameters->PastSceneColour = GraphBuilder.CreateSRV(PrevSceneColourSRVDesc);
		PassParameters->VelocityTexture = GraphBuilder.CreateSRV(VelocityDesc);
		PassParameters->SceneDepth = DepthSRV;
		PassParameters->ViewportMetrics = RTMetrics;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->NormalisedCornerRounding = Rounding;
		PassParameters->MaxSearchSteps = MaxStepOrth;
		PassParameters->MaxDiagonalSearchSteps = MaxStepDiag;
		PassParameters->ReprojectionWeight = ProjectionWeight;
		PassParameters->TemporalHistoryBias = TemporalHistoryBias;
		PassParameters->Resolved = GraphBuilder.CreateUAV(Output.Texture);

		TShaderMapRef<FSMAATemporalResolveCS> ComputeShaderSMAATR(View.ShaderMap, PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("SMAA/TemporalResolve (CS)"), ComputeShaderSMAATR, PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(EdgesTexture->Desc.Extent.X, EdgesTexture->Desc.Extent.Y, 1),
				FIntVector(FSMAATemporalResolveCS::ThreadgroupSizeX,
					FSMAATemporalResolveCS::ThreadgroupSizeY,
					FSMAATemporalResolveCS::ThreadgroupSizeZ)));
	}

	if (!View.bStatePrevViewInfoIsReadOnly)
	{
		//FSMAAHistory& History = View.ViewState->PrevFrameViewInfo.SMAAHistory;
		FSMAAHistory& History = ViewData->SMAAHistory;
		History.SafeRelease();

		GraphBuilder.QueueTextureExtraction(Output.Texture, &History.PastFrame);
		History.ViewportRect = InputRect;
	}

	return Output;
}

FScreenPassTexture AddVisualizeSMAAPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FSMAAInputs& Inputs, const FPostProcessMaterialInputs& InOutInputs, TSharedRef<struct FSMAAViewData> ViewData)
{
	check(Inputs.SceneColor.IsValid());
	check(Inputs.Quality != ESMAAPreset::MAX);
	check(Inputs.EdgeMode != ESMAAEdgeDetectors::MAX);
	RDG_EVENT_SCOPE(GraphBuilder, "SMAA T2x Visualizer");

	FIntPoint InputExtents = Inputs.SceneColor.Texture->Desc.Extent; // View.ViewRect.Size();
	FIntRect InputRect = View.ViewRect;
	InputRect.Min = FIntPoint(0, 0);
	InputRect.Min = InputExtents;
	FIntPoint BackingSize = InputExtents;
	QuantizeSceneBufferSize(InputExtents, BackingSize);

	FIntRect OutputRect = View.ViewRect;
	FIntPoint OutputExtents = View.ViewRect.Size();

	FScreenPassTexture Output = Inputs.OverrideOutput;

	if (!Output.IsValid())
	{
		FRDGTextureDesc WriteOutTextureDesc =
			FRDGTextureDesc::Create2D(BackingSize, PF_FloatRGBA, FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);

		Output = FScreenPassTexture(
			GraphBuilder.CreateTexture(WriteOutTextureDesc, TEXT("SMAA.Output")),
			OutputRect);
	}

	FRHISamplerState* BilinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	FRHISamplerState* PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	auto RTMetrics = FVector4f(1.0 / BackingSize.X, 1.0 / BackingSize.Y, BackingSize.X, BackingSize.Y);

	const FTexture2DResource* AreaResource = ViewData->SMAAAreaTexture;
	if (!AreaResource)
	{
		// Bail
		return Inputs.SceneColor;
	}

	const FTexture2DResource* SearchResource = ViewData->SMAASearchTexture;
	if (!SearchResource)
	{
		// Bail
		return Inputs.SceneColor;
	}

	FRHITexture* AreaTextureRHI = AreaResource->GetTexture2DRHI();
	if (!AreaTextureRHI)
	{
		// Bail
		return Inputs.SceneColor;
	}
	FRDGTextureRef AreaTexture = RegisterExternalTexture(GraphBuilder, AreaTextureRHI, TEXT("SMAA.AreaTexture"));

	FRHITexture* SearchTextureRHI = SearchResource->GetTexture2DRHI();
	if (!SearchTextureRHI)
	{
		// Bail
		return Inputs.SceneColor;
	}
	FRDGTextureRef SearchTexture = RegisterExternalTexture(GraphBuilder, SearchTextureRHI, TEXT("SMAA.SearchTexture"));

	// Create only Edges texture. We're writing straight out to output on blend
	FRDGTextureDesc EdgesTextureDesc =
		FRDGTextureDesc::Create2D(BackingSize, PF_FloatRGBA, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);

	FRDGTextureRef EdgesTexture = GraphBuilder.CreateTexture(EdgesTextureDesc, TEXT("SMAA.EdgesTexture"));

	FRDGTextureRef SceneColor = Inputs.SceneColor.Texture;
	//FRDGTextureRef SceneDepth = Inputs.SceneDepth.Texture;
	FRDGTextureRef Velocity = Inputs.SceneVelocity.Texture;

	// Create Depth SRV Desc
	//FRDGTextureSRVDesc DepthSRVDesc = FRDGTextureSRVDesc::Create(SceneDepth);
	FRDGTextureSRVDesc AreaTextureSRVDesc = FRDGTextureSRVDesc::Create(AreaTexture);
	FRDGTextureSRVDesc SearchTextureSRVDesc = FRDGTextureSRVDesc::Create(SearchTexture);
	FRDGTextureSRVDesc SceneColourSRVDesc = FRDGTextureSRVDesc::Create(SceneColor);
	FRDGTextureSRVDesc EdgesSRVDesc = FRDGTextureSRVDesc::Create(EdgesTexture);
	FRDGTextureSRVDesc WriteOutTextureSRVDesc = FRDGTextureSRVDesc::Create(Output.Texture);
	FRDGTextureSRVDesc VelocityDesc = FRDGTextureSRVDesc::Create(Velocity);

	FRDGTextureSRVRef AreaTextureSRV = GraphBuilder.CreateSRV(AreaTextureSRVDesc);
	FRDGTextureSRVRef SearchTextureSRV = GraphBuilder.CreateSRV(SearchTextureSRVDesc);
	FRDGTextureSRVRef DepthSRV = GraphBuilder.CreateSRV(InOutInputs.SceneTextures.SceneTextures->GetContents()->SceneDepthTexture);
	FRDGTextureSRVRef ColourSRV = GraphBuilder.CreateSRV(SceneColourSRVDesc);

	TRDGTextureAccess<ERHIAccess::SRVCompute> SceneDepth = InOutInputs.SceneTextures.SceneTextures->GetContents()->SceneDepthTexture;

	// Permutations
	ESMAAPreset Preset = Inputs.Quality;
	ESMAAEdgeDetectors EdgeDetectorMode = Inputs.EdgeMode;
	auto Rounding = Inputs.CornerRounding * 0.01f;
	auto MaxStepDiag = Inputs.MaxDiagonalSearchSteps;
	auto MaxStepOrth = Inputs.MaxSearchSteps;
	ESMAAPredicationTexture PredicateSource = Inputs.PredicationSource;
	auto AdaptationFactor = Inputs.AdaptationFactor;
	auto PredicationThreshold = Inputs.PredicationThreshold;
	auto PredicationScale = Inputs.PredicationScale;
	auto PredicationStrength = Inputs.PredicationStrength;

	// Wanted Predicate Texture
	FRDGTextureSRVRef PredicateTexture = GraphBuilder.CreateSRV(GSystemTextures.GetWhiteDummy(GraphBuilder));
	switch (PredicateSource)
	{
		case ESMAAPredicationTexture::Depth:
			PredicateTexture = DepthSRV;
			break;
		case ESMAAPredicationTexture::WorldNormal:
			PredicateTexture = GraphBuilder.CreateSRV(InOutInputs.SceneTextures.SceneTextures->GetContents()->GBufferATexture);
			break;
		case ESMAAPredicationTexture::MRS:
			PredicateTexture = GraphBuilder.CreateSRV(InOutInputs.SceneTextures.SceneTextures->GetContents()->GBufferBTexture);
			break;
		case ESMAAPredicationTexture::None:;
		case ESMAAPredicationTexture::MAX:;
		default:;
	}

	{
		FSMAAEdgeDetectionCS::FPermutationDomain PermutationVector;

		PermutationVector.Set<FSMAAEdgeDetectionCS::FSMAAPresetConfigDim>(Preset);
		PermutationVector.Set<FSMAAEdgeDetectionCS::FSMAAEdgeModeConfigDim>(EdgeDetectorMode);
		PermutationVector.Set<FSMAAEdgeDetectionCS::FSMAAPredicateConfigDim>(ESMAAPredicationTexture::None != PredicateSource);

		FSMAAEdgeDetectionCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FSMAAEdgeDetectionCS::FParameters>();
		FRDGTextureUAVDesc OutputDesc(EdgesTexture);

		PassParameters->DepthTexture = SceneDepth;
		PassParameters->PointTextureSampler = PointClampSampler;
		PassParameters->BilinearTextureSampler = BilinearClampSampler;

		// Pass colour for Depth, Luma, and Colour
		if (EdgeDetectorMode < ESMAAEdgeDetectors::Normal)
		{
			PassParameters->InputSceneColor = ColourSRV;
		}
		else if (ESMAAEdgeDetectors::Normal == EdgeDetectorMode)
		{
			PassParameters->InputSceneColor = GraphBuilder.CreateSRV(InOutInputs.SceneTextures.SceneTextures->GetContents()->GBufferATexture);
		}

		PassParameters->InputDepth = DepthSRV;
		PassParameters->ViewportMetrics = RTMetrics;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->NormalisedCornerRounding = Rounding;
		PassParameters->MaxSearchSteps = MaxStepOrth;
		PassParameters->MaxDiagonalSearchSteps = MaxStepDiag;
		PassParameters->Predicate = PredicateTexture;
		PassParameters->AdaptationFactor = AdaptationFactor;
		PassParameters->PredicationThreshold = PredicationThreshold;
		PassParameters->PredicationScale = PredicationScale;
		PassParameters->PredicationStrength = PredicationStrength;
		PassParameters->EdgesTexture = GraphBuilder.CreateUAV(OutputDesc);

		TShaderMapRef<FSMAAEdgeDetectionCS> ComputeShaderSMAAED(View.ShaderMap, PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("SMAA/EdgeDetection (CS)"), ComputeShaderSMAAED, PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(EdgesTexture->Desc.Extent.X, EdgesTexture->Desc.Extent.Y, 1),
				FIntVector(FSMAAEdgeDetectionCS::ThreadgroupSizeX,
					FSMAAEdgeDetectionCS::ThreadgroupSizeY,
					FSMAAEdgeDetectionCS::ThreadgroupSizeZ)));
	}

	// Blend
	{
		FSMAABlendingWeightsCS::FPermutationDomain PermutationVector;

		PermutationVector.Set<FSMAABlendingWeightsCS::FSMAAPresetConfigDim>(Preset);

		FSMAABlendingWeightsCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FSMAABlendingWeightsCS::FParameters>();

		PassParameters->DepthTexture = SceneDepth;
		PassParameters->PointTextureSampler = TStaticSamplerState<SF_Point>::GetRHI();
		PassParameters->BilinearTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
		PassParameters->AreaTexture = AreaTextureSRV;
		PassParameters->InputEdges = GraphBuilder.CreateSRV(EdgesSRVDesc);
		PassParameters->TemporalJitterPixels = FVector2f(View.TemporalJitterPixels);
		PassParameters->SubpixelWeights = SubpixelJitterWeights[View.TemporalJitterIndex & 1];
		PassParameters->SearchTexture = SearchTextureSRV;
		PassParameters->ViewportMetrics = RTMetrics;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->NormalisedCornerRounding = Rounding;
		PassParameters->MaxSearchSteps = MaxStepOrth;
		PassParameters->MaxDiagonalSearchSteps = MaxStepDiag;
		PassParameters->BlendTexture = GraphBuilder.CreateUAV(Output.Texture);

		TShaderMapRef<FSMAABlendingWeightsCS> ComputeShaderSMAABW(View.ShaderMap, PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("SMAA/BlendWeights (CS)"), ComputeShaderSMAABW, PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(EdgesTexture->Desc.Extent.X, EdgesTexture->Desc.Extent.Y, 1),
				FIntVector(FSMAABlendingWeightsCS::ThreadgroupSizeX,
					FSMAABlendingWeightsCS::ThreadgroupSizeY,
					FSMAABlendingWeightsCS::ThreadgroupSizeZ)));
	}

	return Output;
}
UE_ENABLE_OPTIMIZATION
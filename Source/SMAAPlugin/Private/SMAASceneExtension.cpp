// Fill out your copyright notice in the Description page of Project Settings.


#include "SMAASceneExtension.h"

#include "SceneView.h"
#include "ScreenPass.h"
#include "CommonRenderResources.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/PostProcessMaterial.h"
#include "ScenePrivate.h"
#include "Engine/TextureRenderTarget2D.h"
#include "DynamicResolutionState.h"
#include "FXRenderingUtils.h"
#include "Rendering/Texture2DResource.h"

#include "PostProcess/PostProcessSMAA.h"

TAutoConsoleVariable<int32> CVarSMAAEnabled(
	TEXT("r.SMAA"), 0,
	TEXT(" 0 - off\n")
	TEXT(" 1 - on"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSMAAVisualizeEnabled(
	TEXT("r.SMAA.Visualize"), 0,
	TEXT(" 0 - off\n")
	TEXT(" 1 - on"),
	ECVF_RenderThreadSafe);

FSMAASceneExtension::FSMAASceneExtension(const FAutoRegister& AutoReg, FTexture2DResource* InSMAAAreaTexture, FTexture2DResource* InSMAASearchTexture)
	: FSceneViewExtensionBase(AutoReg)
	, SMAAAreaTexture(InSMAAAreaTexture)
	, SMAASearchTexture(InSMAASearchTexture)
{
	//check(SMAAAreaTexture)
}

void FSMAASceneExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	GetOrCreateViewData(InView);

	//check(InView.bIsViewInfo);
	//GEngine->AddOnScreenDebugMessage(-1, 0.f, FColor::Red, FString::Printf(L"Jitter index: %d", ((FViewInfo&)InView).TemporalJitterIndex));

	//InView.ViewMatrices.HackAddTemporalAAProjectionJitter(FVector2D(1.f, 1.f));
}

TSharedPtr<FSMAAViewData> FSMAASceneExtension::GetOrCreateViewData(const FSceneView& InView)
{
	if (InView.State == nullptr)
	{
		return nullptr;
	}

	const uint32 Index = InView.State->GetViewKey();
	TSharedPtr<FSMAAViewData>& ViewData = ViewDataMap.FindOrAdd(Index);
	if (!ViewData.IsValid())
	{
		ViewData = MakeShared<FSMAAViewData>();
		ViewData->SMAAAreaTexture = SMAAAreaTexture;
		ViewData->SMAASearchTexture = SMAASearchTexture;
		ViewData->JitterIndex = 0;
	}
	return ViewData;
}

bool FSMAASceneExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return CVarSMAAEnabled.GetValueOnAnyThread() == 1;
}

void FSMAASceneExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
	check(InView.bIsViewInfo);

	FViewInfo& View = (FViewInfo&)InView;
	if (InView.State == nullptr)
	{
		return;
	}
	FSceneViewState* ViewState = InView.State->GetConcreteViewState();

	ApplyJitter(View, ViewState, InView.UnconstrainedViewRect, GetOrCreateViewData(InView).ToSharedRef());
}

void FSMAASceneExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass,
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 5
	const FSceneView& View,
#endif
	FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	if (Pass == EPostProcessingPass::FXAA)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FSMAASceneExtension::PostProcessPass_RenderThread, Pass));
	}
}

FScreenPassTexture FSMAASceneExtension::PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& InOutInputs, EPostProcessingPass Pass)
{
	InOutInputs.Validate();

	SMAAAreaTexture->InitRHI(GetImmediateCommandList_ForRenderCommand());
	SMAASearchTexture->InitRHI(GetImmediateCommandList_ForRenderCommand());

	auto& SceneTextureParameters = InOutInputs.SceneTextures.SceneTextures;

	{
		auto PredicateSource = GetPredicateSource();

		FSMAAInputs PassInputs;
		//PassSequence.AcceptOverrideIfLastPass(EPass::SMAA, PassInputs.OverrideOutput);
		PassInputs.SceneColor = FScreenPassTexture(InOutInputs.GetInput(EPostProcessMaterialInput::SceneColor));
		PassInputs.SceneVelocity = FScreenPassTexture(InOutInputs.GetInput(EPostProcessMaterialInput::Velocity));
		PassInputs.Quality = GetSMAAPreset();
		PassInputs.EdgeMode = GetSMAAEdgeDetectors();
		PassInputs.PredicationSource = PredicateSource;
		PassInputs.MaxSearchSteps = GetSMAAMaxSearchSteps();
		PassInputs.MaxDiagonalSearchSteps = GetSMAAMaxDiagonalSearchSteps();
		PassInputs.CornerRounding = GetSMAACornerRounding();
		PassInputs.AdaptationFactor = GetSMAAAdaptationFactor();
		PassInputs.ReprojectionWeight = GetSMAAReprojectionWeight();
		PassInputs.PredicationThreshold = GetSMAAPredicationThreshold();
		PassInputs.PredicationScale = GetSMAAPredicationScale();
		PassInputs.PredicationStrength = GetSMAAPredicationStrength();
		PassInputs.TemporalHistoryBias = GetSMAATemporalHistoryBias();

		check(View.bIsViewInfo);

		auto ViewData = GetOrCreateViewData(View);
		if (!ViewData.IsValid())
		{
			return FScreenPassTexture(InOutInputs.GetInput(EPostProcessMaterialInput::SceneColor));
		}

		if (CVarSMAAVisualizeEnabled.GetValueOnAnyThread() == 1)
		{
			auto SceneColorSlice = FScreenPassTextureSlice::CreateFromScreenPassTexture(GraphBuilder, AddVisualizeSMAAPasses(GraphBuilder, (const FViewInfo&)View, PassInputs, InOutInputs, ViewData.ToSharedRef()));
			return FScreenPassTexture(SceneColorSlice);
		}
		else
		{
			auto SceneColorSlice = FScreenPassTextureSlice::CreateFromScreenPassTexture(GraphBuilder, AddSMAAPasses(GraphBuilder, (const FViewInfo&)View, PassInputs, InOutInputs, ViewData.ToSharedRef()));
			return FScreenPassTexture(SceneColorSlice);
		}
	}

	return FScreenPassTexture(InOutInputs.GetInput(EPostProcessMaterialInput::SceneColor));
}

void FSMAASceneExtension::ApplyJitter(FViewInfo& View, FSceneViewState* ViewState, FIntRect ViewRect, TSharedRef<FSMAAViewData> ViewData)
{
	float EffectivePrimaryResolutionFraction = 1.f;// float(ViewRect.Width()) / float(View.GetSecondaryViewRectSize().X);

	// Compute number of TAA samples.
	int32 TemporalAASamples = 2;
	{
		//if (TAAConfig == EMainTAAPassConfig::TSR)
		//{
		//	// Force the number of AA sample to make sure the quality doesn't get
		//	// compromised by previously set settings for Gen4 TAA
		//	TemporalAASamples = 8;
		//}
		//else
		//{
		//	TemporalAASamples = FMath::Clamp(CVarTemporalAASamplesValue, 1, 255);
		//}

		//if (bTemporalUpsampling)
		//{
		//	// When doing TAA upsample with screen percentage < 100%, we need extra temporal samples to have a
		//	// constant temporal sample density for final output pixels to avoid output pixel aligned converging issues.
		//	TemporalAASamples = FMath::RoundToInt(float(TemporalAASamples) * FMath::Max(1.f, 1.f / (EffectivePrimaryResolutionFraction * EffectivePrimaryResolutionFraction)));
		//}
		//else if (CVarTemporalAASamplesValue == 5)
		//{
		//	TemporalAASamples = 4;
		//}

		// Use immediately higher prime number to break up coherence between the TAA jitter sequence and any
		// other random signal that are power of two of View.StateFrameIndex
		//if (TAAConfig == EMainTAAPassConfig::TSR)
		//{
		//	static const uint8 kFirstPrimeNumbers[] = {
		//		2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97,
		//		101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197, 199,
		//		211, 223, 227, 229, 233, 239, 241, 251,
		//	};

		//	for (int32 PrimeNumberId = FMath::Max(4, (TemporalAASamples - 1) / 5); PrimeNumberId < UE_ARRAY_COUNT(kFirstPrimeNumbers); PrimeNumberId++)
		//	{
		//		if (int32(kFirstPrimeNumbers[PrimeNumberId]) >= TemporalAASamples)
		//		{
		//			TemporalAASamples = int32(kFirstPrimeNumbers[PrimeNumberId]);
		//			break;
		//		}
		//	}
		//}
	}

	// Compute the new sample index in the temporal sequence.
	int32 TemporalSampleIndex = ViewData->JitterIndex + 1;
	if (TemporalSampleIndex >= TemporalAASamples || View.bCameraCut)
	{
		TemporalSampleIndex = 0;
	}

	//#if !UE_BUILD_SHIPPING
	//	if (CVarTAADebugOverrideTemporalIndex.GetValueOnRenderThread() >= 0)
	//	{
	//		TemporalSampleIndex = CVarTAADebugOverrideTemporalIndex.GetValueOnRenderThread();
	//	}
	//#endif

	// Updates view state.
	//if (!View.bStatePrevViewInfoIsReadOnly)// && !bFreezeTemporalSequences)
	{
		ViewState->TemporalAASampleIndex = TemporalSampleIndex;
		ViewData->JitterIndex = TemporalSampleIndex;
	}

	float SamplesX[] = { -4.0f / 16.0f, 4.0 / 16.0f };
	float SamplesY[] = { -4.0f / 16.0f, 4.0 / 16.0f };

	check(TemporalAASamples == UE_ARRAY_COUNT(SamplesX));
	float SampleX = SamplesX[TemporalSampleIndex];
	float SampleY = SamplesY[TemporalSampleIndex];

	View.TemporalJitterSequenceLength = TemporalAASamples;
	View.TemporalJitterIndex = TemporalSampleIndex;
	//View.TemporalJitterPixels.X = SampleX;
	//View.TemporalJitterPixels.Y = SampleY;

	View.ViewMatrices.HackAddTemporalAAProjectionJitter(FVector2D(SampleX * 1.f / ViewRect.Width(), SampleY * -1.f / ViewRect.Height()));
}


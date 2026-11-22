#include "CoreMinimal.h"
#include "CanvasItem.h"
#include "Containers/Ticker.h"
#include "Debug/DebugDrawService.h"
#include "DynamicRHI.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntimeRDG.h"
#include "PostProcess/PostProcessUpscale.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "SceneRendering.h"
#include "SceneViewExtension.h"
#include "ShaderParameterStruct.h"

DEFINE_LOG_CATEGORY_STATIC(LogAdventureSR, Log, All);

namespace
{
using namespace UE::NNE;

const FIntPoint InputSize(1920, 1080);
const FIntPoint OutputSize(3840, 2160);
constexpr uint32 InputDimensions[] = {1, 3, 1080, 1920};
constexpr uint32 OutputDimensions[] = {1, 3, 2160, 3840};
constexpr TCHAR TensorRTRuntimeName[] = TEXT("NNERuntimeTRT");
constexpr TCHAR ModelPath[] = TEXT("/Game/ML/cnnps_feat16_conv8_scale2_1x3x1080x1920_static_fp32_op20.cnnps_feat16_conv8_scale2_1x3x1080x1920_static_fp32_op20");

TAutoConsoleVariable<int32> CVarAdventureSR(
	TEXT("r.AdventureSR"), 1,
	TEXT("Use TensorRT-RTX for the 1920x1080 to 3840x2160 SDR primary spatial upscale."),
	ECVF_RenderThreadSafe);

FCriticalSection ViewportStatusLock;
TMap<const FRenderTarget*, uint64> LastUpscaleFrameByViewport;

void DrawViewportStatus(UCanvas* Canvas, APlayerController*)
{
	const FSceneView* View = Canvas->SceneView;
	if (!View || !View->Family || View->Family->Views.IsEmpty() || View != View->Family->Views[0])
	{
		return;
	}

	bool bIsActive = false;
	{
		FScopeLock Lock(&ViewportStatusLock);
		for (auto It = LastUpscaleFrameByViewport.CreateIterator(); It; ++It)
		{
			// The game thread draws this overlay while the render thread can be a frame behind.
			if (It.Value() + 2 < View->Family->FrameCounter)
			{
				It.RemoveCurrent();
			}
		}
		bIsActive = LastUpscaleFrameByViewport.Contains(View->Family->RenderTarget);
	}

	const TCHAR* Label = TEXT("SR: OFF");
	FLinearColor Color = FLinearColor::Red;
	if (CVarAdventureSR.GetValueOnGameThread() != 0)
	{
		Label = bIsActive ? TEXT("SR: ACTIVE (TensorRT-RTX)") : TEXT("SR: ON (inactive)");
		Color = bIsActive ? FLinearColor::Green : FLinearColor::Yellow;
	}

	FCanvasTextItem Text(FVector2D(24, 24), FText::FromString(Label), GEngine->GetMediumFont(), Color);
	const float Scale = FMath::Clamp(Canvas->SizeY / 1080.0f, 1.0f, 2.0f);
	Text.Scale = FVector2D(Scale, Scale);
	Text.EnableShadow(FLinearColor::Black);
	Canvas->DrawItem(Text);
}

class FPackInputCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FPackInputCS);
	SHADER_USE_PARAMETER_STRUCT(FPackInputCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, InputOrigin)
		SHADER_PARAMETER(uint32, ConvertSRGB)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, PackedTensor)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

class FUnpackOutputCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FUnpackOutputCS);
	SHADER_USE_PARAMETER_STRUCT(FUnpackOutputCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, InputOrigin)
		SHADER_PARAMETER(uint32, ConvertSRGB)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, OutputTensor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

IMPLEMENT_GLOBAL_SHADER(FPackInputCS, "/AdventureSuperResolution/AdventureSuperResolution.usf", "PackInput", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUnpackOutputCS, "/AdventureSuperResolution/AdventureSuperResolution.usf", "UnpackOutput", SF_Compute);

TSharedPtr<IModelInstanceRDG, ESPMode::ThreadSafe> CreateModelInstance()
{
	const TWeakInterfacePtr<INNERuntimeRDG> Runtime = GetRuntime<INNERuntimeRDG>(TensorRTRuntimeName);
	if (!Runtime.IsValid())
	{
		UE_LOG(LogAdventureSR, Warning, TEXT("%s unavailable. Registered RDG runtimes: %s"),
			TensorRTRuntimeName, *FString::Join(GetAllRuntimeNames<INNERuntimeRDG>(), TEXT(", ")));
		return nullptr;
	}

	UNNEModelData* ModelData = LoadObject<UNNEModelData>(nullptr, ModelPath);
	if (!ModelData || Runtime->CanCreateModelRDG(ModelData) != EResultStatus::Ok)
	{
		UE_LOG(LogAdventureSR, Warning, TEXT("%s cannot create a model from %s"), TensorRTRuntimeName, ModelPath);
		return nullptr;
	}

	const TSharedPtr<IModelRDG> Model = Runtime->CreateModelRDG(ModelData);
	TSharedPtr<IModelInstanceRDG> Instance = Model ? Model->CreateModelInstanceRDG() : nullptr;
	if (!Instance)
	{
		UE_LOG(LogAdventureSR, Warning, TEXT("Failed to create the TensorRT-RTX RDG model instance."));
		return nullptr;
	}

	const FTensorShape InputShape = FTensorShape::Make(InputDimensions);
	const FTensorShape OutputShape = FTensorShape::Make(OutputDimensions);
	const auto Inputs = Instance->GetInputTensorDescs();
	const auto Outputs = Instance->GetOutputTensorDescs();
	if (Inputs.Num() != 1 || Outputs.Num() != 1
		|| Inputs[0].GetDataType() != ENNETensorDataType::Float
		|| Outputs[0].GetDataType() != ENNETensorDataType::Float
		|| !InputShape.IsCompatibleWith(Inputs[0].GetShape())
		|| !OutputShape.IsCompatibleWith(Outputs[0].GetShape())
		|| Instance->SetInputTensorShapes({InputShape}) != EResultStatus::Ok)
	{
		UE_LOG(LogAdventureSR, Warning, TEXT("TensorRT-RTX did not accept the expected FP32 NCHW shapes."));
		return nullptr;
	}

	UE_LOG(LogAdventureSR, Display, TEXT("TensorRT-RTX RDG ready for 1920x1080 -> 3840x2160."));
	return Instance;
}

FRDGTextureRef AddSuperResolutionPasses(
	FRDGBuilder& GraphBuilder,
	const FGlobalShaderMap* ShaderMap,
	IModelInstanceRDG& Instance,
	const FScreenPassTexture& Input,
	EPixelFormat OutputFormat)
{
	const auto OutputShapes = Instance.GetOutputTensorShapes();
	if (OutputShapes.Num() != 1 || OutputShapes[0] != FTensorShape::Make(OutputDimensions))
	{
		return nullptr;
	}

	const uint32 ConvertSRGB = EnumHasAnyFlags(Input.Texture->Desc.Flags, TexCreate_SRGB) ? 1 : 0;
	const FRDGBufferRef InputTensor = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), InputSize.X * InputSize.Y * 3), TEXT("AdventureSR.InputTensor"));
	const FRDGBufferRef OutputTensor = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), OutputSize.X * OutputSize.Y * 3), TEXT("AdventureSR.OutputTensor"));

	auto* Pack = GraphBuilder.AllocParameters<FPackInputCS::FParameters>();
	Pack->InputOrigin = Input.ViewRect.Min;
	Pack->ConvertSRGB = ConvertSRGB;
	Pack->InputTexture = Input.Texture;
	Pack->PackedTensor = GraphBuilder.CreateUAV(InputTensor, PF_R32_FLOAT);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("AdventureSR Pack 1080p"), TShaderMapRef<FPackInputCS>(ShaderMap),
		Pack, FComputeShaderUtils::GetGroupCount(InputSize, 8));

	const FTensorBindingRDG InputBinding{InputTensor};
	const FTensorBindingRDG OutputBinding{OutputTensor};
	if (Instance.EnqueueRDG(GraphBuilder, {InputBinding}, {OutputBinding}) != EResultStatus::Ok)
	{
		return nullptr;
	}

	const FRDGTextureRef Upscaled = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(OutputSize, OutputFormat, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("AdventureSR.Upscaled"));
	auto* Unpack = GraphBuilder.AllocParameters<FUnpackOutputCS::FParameters>();
	Unpack->InputOrigin = Input.ViewRect.Min;
	Unpack->ConvertSRGB = ConvertSRGB;
	Unpack->InputTexture = Input.Texture;
	Unpack->OutputTensor = GraphBuilder.CreateSRV(OutputTensor, PF_R32_FLOAT);
	Unpack->OutputTexture = GraphBuilder.CreateUAV(Upscaled);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("AdventureSR Unpack 4K"), TShaderMapRef<FUnpackOutputCS>(ShaderMap),
		Unpack, FComputeShaderUtils::GetGroupCount(OutputSize, 8));
	return Upscaled;
}

class FAdventureSpatialUpscaler final : public ISpatialUpscaler
{
public:
	explicit FAdventureSpatialUpscaler(TSharedRef<IModelInstanceRDG> InInstance)
		: Instance(MoveTemp(InInstance))
	{
	}

	const TCHAR* GetDebugName() const override { return TEXT("Adventure TensorRT-RTX"); }

	ISpatialUpscaler* Fork_GameThread(const FSceneViewFamily&) const override
	{
		return new FAdventureSpatialUpscaler(Instance);
	}

	FScreenPassTexture AddPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FInputs& Inputs) const override
	{
		if (Inputs.Stage == EUpscaleStage::PrimaryToOutput && !View.Family->bIsHDR
			&& !View.LensDistortionLUT.IsEnabled())
		{
			const FIntPoint ActualInputSize = Inputs.SceneColor.ViewRect.Size();
			const FIntPoint ActualOutputSize = View.GetSecondaryViewRectSize();
			const FIntPoint TargetSize = Inputs.OverrideOutput.IsValid() ? Inputs.OverrideOutput.ViewRect.Size() : ActualOutputSize;
			if (ActualInputSize != InputSize || ActualOutputSize != OutputSize || TargetSize != OutputSize)
			{
				static FIntPoint LastInputSize = FIntPoint::ZeroValue;
				static FIntPoint LastOutputSize = FIntPoint::ZeroValue;
				static FIntPoint LastTargetSize = FIntPoint::ZeroValue;
				if (ActualInputSize != LastInputSize || ActualOutputSize != LastOutputSize || TargetSize != LastTargetSize)
				{
					LastInputSize = ActualInputSize;
					LastOutputSize = ActualOutputSize;
					LastTargetSize = TargetSize;
					UE_LOG(LogAdventureSR, Display, TEXT("Skipping TensorRT-RTX upscale: scene input %dx%d, view output %dx%d, render target %dx%d; expected %dx%d input and %dx%d output."),
						ActualInputSize.X, ActualInputSize.Y, ActualOutputSize.X, ActualOutputSize.Y, TargetSize.X, TargetSize.Y,
						InputSize.X, InputSize.Y, OutputSize.X, OutputSize.Y);
				}
			}
			else if (FRDGTextureRef Texture = AddSuperResolutionPasses(
				GraphBuilder, View.ShaderMap, *Instance, Inputs.SceneColor, PF_FloatRGBA))
			{
				{
					FScopeLock Lock(&ViewportStatusLock);
					uint64& LastFrame = LastUpscaleFrameByViewport.FindOrAdd(View.Family->RenderTarget);
					LastFrame = FMath::Max(LastFrame, View.Family->FrameCounter);
				}
				static bool bLoggedUpscale = false;
				if (!bLoggedUpscale)
				{
					bLoggedUpscale = true;
					UE_LOG(LogAdventureSR, Display, TEXT("Viewport enqueued the 1920x1080 -> 3840x2160 TensorRT-RTX upscale."));
				}
				const FScreenPassTexture Output(Texture, FIntRect(FIntPoint::ZeroValue, OutputSize));
				if (Inputs.OverrideOutput.IsValid())
				{
					AddDrawTexturePass(GraphBuilder, View, Output, Inputs.OverrideOutput);
					return Inputs.OverrideOutput;
				}
				return Output;
			}
		}
		const IConsoleVariable* Quality = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Upscale.Quality"));
		const int32 Method = FMath::Clamp(Quality ? Quality->GetInt() + 1 : 4, 1, static_cast<int32>(EUpscaleMethod::Gaussian));
		return AddDefaultUpscalePass(GraphBuilder, View, Inputs, static_cast<EUpscaleMethod>(Method), View.LensDistortionLUT);
	}

private:
	TSharedRef<IModelInstanceRDG> Instance;
};

class FAdventureSRViewExtension final : public FSceneViewExtensionBase
{
public:
	explicit FAdventureSRViewExtension(const FAutoRegister& AutoRegister)
		: FSceneViewExtensionBase(AutoRegister)
	{
	}

	void BeginRenderViewFamily(FSceneViewFamily& Family) override
	{
		if (!CVarAdventureSR.GetValueOnGameThread() || !Family.EngineShowFlags.Game
			|| Family.bIsHDR || Family.Views.Num() != 1 || Family.GetPrimarySpatialUpscalerInterface())
		{
			return;
		}

		const FSceneView& View = *Family.Views[0];
		if (View.bIsSceneCapture || View.bIsReflectionCapture)
		{
			return;
		}

		const FIntPoint ViewportSize = View.UnscaledViewRect.Size();
		if (ViewportSize != OutputSize)
		{
			if (ViewportSize != LastMismatchedViewportSize)
			{
				LastMismatchedViewportSize = ViewportSize;
				UE_LOG(LogAdventureSR, Display, TEXT("Skipping TensorRT-RTX upscale: game viewport is %dx%d; expected %dx%d output."),
					ViewportSize.X, ViewportSize.Y, OutputSize.X, OutputSize.Y);
			}
			return;
		}

		if (View.PrimaryScreenPercentageMethod != EPrimaryScreenPercentageMethod::SpatialUpscale)
		{
			return;
		}

		if (!bTriedModel)
		{
			bTriedModel = true;
			Instance = CreateModelInstance();
		}
		if (Instance)
		{
			Family.SetPrimarySpatialUpscalerInterface(new FAdventureSpatialUpscaler(Instance.ToSharedRef()));
		}
	}

private:
	TSharedPtr<IModelInstanceRDG> Instance;
	FIntPoint LastMismatchedViewportSize = FIntPoint::ZeroValue;
	bool bTriedModel = false;
};

#if !UE_BUILD_SHIPPING
bool RunSmokeTest()
{
	const TSharedPtr<IModelInstanceRDG> Instance = CreateModelInstance();
	if (!Instance)
	{
		return false;
	}

	bool bPassed = false;
	ENQUEUE_RENDER_COMMAND(AdventureSRSmoke)([Instance, &bPassed](FRHICommandListImmediate& RHICmdList)
	{
		// TensorRT-RTX publishes the concrete shapes from its D3D12 queue callback.
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		FRHIGPUTextureReadback Readback(TEXT("AdventureSR.SmokeReadback"));
		FRDGBuilder GraphBuilder(RHICmdList);
		const FRDGTextureRef TestInput = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(InputSize + FIntPoint(16, 16), PF_A32B32G32R32F,
				FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("AdventureSR.SmokeInput"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TestInput), FLinearColor(0.2f, 0.5f, 0.8f, 0.75f));
		const FIntPoint Origin(8, 8);
		const FRDGTextureRef Result = AddSuperResolutionPasses(GraphBuilder, GetGlobalShaderMap(GMaxRHIFeatureLevel),
			*Instance, FScreenPassTexture(TestInput, FIntRect(Origin, Origin + InputSize)), PF_A32B32G32R32F);
		if (Result)
		{
			AddEnqueueCopyPass(GraphBuilder, &Readback, Result);
		}
		GraphBuilder.Execute();
		if (!Result)
		{
			return;
		}

		RHICmdList.SubmitAndBlockUntilGPUIdle();
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		if (!Readback.IsReady())
		{
			return;
		}

		int32 RowPitch = 0;
		const FVector4f* Pixels = static_cast<const FVector4f*>(Readback.Lock(RowPitch));
		if (Pixels)
		{
			const FVector4f Corner = Pixels[0];
			const FVector4f Center = Pixels[1080 * RowPitch + 1920];
			// Reference pixels from ONNX Runtime CPU for the same RGB input.
			bPassed = Corner.Equals(FVector4f(0.203657f, 0.508921f, 0.812509f, 0.75f), 0.002f)
				&& Center.Equals(FVector4f(0.200204f, 0.499878f, 0.800068f, 0.75f), 0.002f);
			UE_LOG(LogAdventureSR, Display, TEXT("Smoke pixels: corner %s; center %s"), *Corner.ToString(), *Center.ToString());
			Readback.Unlock();
		}
	});
	FlushRenderingCommands();
	return bPassed;
}
#endif

class FAdventureSuperResolutionModule final : public IModuleInterface
{
	void StartupModule() override
	{
		AddShaderSourceDirectoryMapping(TEXT("/AdventureSuperResolution"),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders"))));
		PostEngineInit = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FAdventureSuperResolutionModule::OnEngineInit);
		EnginePreExit = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FAdventureSuperResolutionModule::ReleaseViewportIntegration);
	}

	void ShutdownModule() override
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInit);
		FCoreDelegates::OnEnginePreExit.Remove(EnginePreExit);
		ReleaseViewportIntegration();
	}

	void ReleaseViewportIntegration()
	{
		if (ViewportStatusDraw.IsValid())
		{
			UDebugDrawService::Unregister(ViewportStatusDraw);
			ViewportStatusDraw.Reset();
		}
		if (ViewExtension)
		{
			FlushRenderingCommands();
			ViewExtension.Reset();
		}
	}

	void OnEngineInit()
	{
		const bool bCanUseSR = GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::D3D12
			&& GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM6;
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("AdventureSRSmoke")))
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([bCanUseSR](float)
			{
				const bool bPassed = bCanUseSR && RunSmokeTest();
				UE_LOG(LogAdventureSR, Display, TEXT("TensorRT-RTX RDG smoke test: %s"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
				FPlatformMisc::RequestExitWithStatus(false, bPassed ? 0 : 1);
				return false;
			}));
			return;
		}
#endif
		if (!IsRunningCommandlet() && !IsRunningDedicatedServer())
		{
			ViewportStatusDraw = UDebugDrawService::Register(TEXT("Game"), FDebugDrawDelegate::CreateStatic(&DrawViewportStatus));
			if (bCanUseSR)
			{
				ViewExtension = FSceneViewExtensions::NewExtension<FAdventureSRViewExtension>();
			}
		}
	}

	FDelegateHandle PostEngineInit;
	FDelegateHandle EnginePreExit;
	FDelegateHandle ViewportStatusDraw;
	TSharedPtr<FAdventureSRViewExtension, ESPMode::ThreadSafe> ViewExtension;
};
}

IMPLEMENT_MODULE(FAdventureSuperResolutionModule, AdventureSuperResolution);

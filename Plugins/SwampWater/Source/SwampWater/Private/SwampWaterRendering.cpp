#include "SwampWaterRendering.h"
#include "SwampWaterSim.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "PipelineStateCache.h"
#include "CommonRenderResources.h"
#include "PostProcess/PostProcessInputs.h"
#include "SceneRendering.h"
#include "SceneTextureParameters.h"
#include "GlobalRenderResources.h"
#include "SystemTextures.h"
#include "TextureResource.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "SwampWaterSubsystem.h"
#include "SceneManagement.h"

static TAutoConsoleVariable<int32> CVarSwampWaterPlanar(TEXT("swampwater.Planar"), 1, TEXT("Planar (mirror camera) reflection of the water: 1 on, 0 off (screen-space reflection instead)."));
static TAutoConsoleVariable<float> CVarSwampWaterPlanarScale(TEXT("swampwater.Planar.ScreenPercentage"), 50.f, TEXT("Resolution of the water planar reflection, % of the view."));
static TAutoConsoleVariable<int32> CVarSwampWaterPlanarDebug(TEXT("swampwater.Planar.Debug"), 0, TEXT("1: water shows only the planar reflection capture (magenta = none)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarSwampWaterEyeAdaptStops(TEXT("swampwater.UnderwaterAdaptStops"), 2.5f,
	TEXT("How many stops the exposure may brighten under water beyond the last above-water exposure (eye pupil ~2.5)."));
static TAutoConsoleVariable<float> CVarSwampWaterWhiteAdapt(TEXT("swampwater.UnderwaterWhiteAdapt"), -1.f,
	TEXT("Degree of the eye's chromatic adaptation to the light under water: 0 none, 1 full, -1 from CIECAM02 by the adapting luminance."));
static TAutoConsoleVariable<int32> CVarSwampWaterSunVisDebug(TEXT("swampwater.SunVisDebug"), 0,
	TEXT("1: the water surface shows the sun visibility 25 cm under it (white = lit, black = shadow); 2: the bottom silt (red 100 mg/l, green 20)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarSwampWaterSSR(TEXT("swampwater.SSR"), 24, TEXT("Steps of the water screen-space reflection ray (0 = sky only)."), ECVF_RenderThreadSafe);

DECLARE_GPU_STAT(WaterRender);
DECLARE_GPU_STAT(WaterCopies);
DECLARE_GPU_STAT(WaterSurface);
DECLARE_GPU_STAT(WaterWhite);
DECLARE_GPU_STAT(WaterLight);

// Плоское отражение: камера в зеркальной точке под плоскостью воды (тот же поворот с обратным наклоном, та же проекция)
// видит сквозь точку воды ровно то, что вода отражает к глазу. Под водой всё срезает плоскость отсечения. Ставится
// по камере этого кадра до сцены — снимок рисуется перед основным видом того же кадра, без отставания.
// Под водой гумус оставляет проценты света: изнутри болото тёмное. Автоэкспозиция движка доводит любую сцену до
// средне-серого и делала толщу ярко-оранжевой — ярче, чем над водой. Глаз так не может: зрачок (днём ~3 мм, в темноте
// до ~7 мм) даёт ~2.5 ступени за секунды, палочки — минуты. Под водой экспозиции позволено подняться над той, к
// которой она пришла над водой, не больше чем на swampwater.UnderwaterAdaptStops (нижняя граница её белой точки).
static constexpr float WhiteAdaptTime = 1.5f; // с: постоянная времени цветовой адаптации глаза

void FSwampWaterViewExtension::LimitUnderwaterAdaptation(FSceneView& InView)
{
	FFinalPostProcessSettings& PP = InView.FinalPostProcessSettings;
	static const IConsoleVariable* ExtendedVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange"));
	static const IConsoleVariable* LensVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptation.LensAttenuation"));
	const bool bExtended = ExtendedVar && ExtendedVar->GetInt() != 0;
	const float LuminanceMax = bExtended ? 0.78f / FMath::Max(LensVar ? LensVar->GetFloat() : 0.78f, 0.01f) : 1.f;

	const FVector Eye = InView.ViewMatrices.GetViewOrigin();
	float Level = 0.f;
	FVector2D Flow;
	const USwampWaterSubsystem* Water = Owner.Get();
	const bool bUnder = Water && Water->SampleWater(Eye, Level, Flow) && Eye.Z < Level;

	// Цвет: в одноцветном свете глаз за секунды частично принимает его за белый (хроматическая адаптация). Свет у
	// глаза — сверху сквозь его глубину и ~полметра пути до того, что он видит. Степень адаптации — по CIECAM02
	// (CIE 159:2004): D = F·[1 − (1/3.6)·exp((−L_A − 42)/92)], L_A — яркость поля адаптации (кд/м², средняя по кадру),
	// F = 1 (обычное окружение). Адаптация — балансом белого движка (WhiteTemp/WhiteTint: переход в пространство колбочек
	// и обратно, TonemapCommon WhiteBalance): «белое» — цветность света воды, частично сдвинутая к D65 на D.
	const double Now = InView.Family ? InView.Family->Time.GetRealTimeSeconds() : 0.0;
	const float Dt = LastAdaptTime < 0.0 ? 1.f : FMath::Clamp(float(Now - LastAdaptTime), 0.f, 1.f);
	LastAdaptTime = Now;
	FVector2f TargetUV = WhiteUVD65;
	if (bUnder)
	{
		const float Path = FMath::Max(Level - float(Eye.Z), 0.f) * 0.01f + 0.5f;
		// Спектр света у глаза (доли D65): гаснет как exp(−(a + bb)·путь) в каждой полосе → XYZ → CIE 1960 uv.
		float X = 0.f, Y = 0.f, Z = 0.f;
		for (int32 Band = 0; Band < SwampWaterOptics::Bands; Band++)
		{
			const float Light = FMath::Exp(-(SwampWaterOptics::SpecAbsorption[Band] + SwampWaterOptics::Backscatter) * Path);
			X += SwampWaterOptics::SpecToXYZ[0][Band] * Light;
			Y += SwampWaterOptics::SpecToXYZ[1][Band] * Light;
			Z += SwampWaterOptics::SpecToXYZ[2][Band] * Light;
		}
		const float Den = X + 15.f * Y + 3.f * Z;
		const FVector2f IllumUV(4.f * X / Den, 6.f * Y / Den);
		float Degree = CVarSwampWaterWhiteAdapt.GetValueOnGameThread();
		if (Degree < 0.f)
		{
			const float LA = InView.State ? FMath::Max(InView.State->GetLastAverageSceneLuminance(), 0.f) : 50.f;
			Degree = 1.f - (1.f / 3.6f) * FMath::Exp((-LA - 42.f) / 92.f);
		}
		TargetUV = FMath::Lerp(WhiteUVD65, IllumUV, FMath::Clamp(Degree, 0.f, 1.f));
	}
	AdaptUV = FMath::Lerp(AdaptUV, TargetUV, 1.f - FMath::Exp(-Dt / WhiteAdaptTime));
	if (!AdaptUV.Equals(WhiteUVD65, 1e-4f))
	{
		// uv → xy → температура (McCamy 1992), оттенок — смещение по изотерме от локуса Планка (Krystek 1985), как
		// их понимает WhiteBalance движка: Tint·0.05 в uv.
		const float Ux = AdaptUV.X, Vx = AdaptUV.Y;
		const float x = 3.f * Ux / (2.f * Ux - 8.f * Vx + 4.f), y = 2.f * Vx / (2.f * Ux - 8.f * Vx + 4.f);
		const float n = (x - 0.3320f) / (y - 0.1858f);
		const float T = FMath::Clamp(-449.f * n * n * n + 3525.f * n * n - 6823.3f * n + 5520.33f, 1500.f, 15000.f);
		const float Pu = (0.860117757f + 1.54118254e-4f * T + 1.28641212e-7f * T * T) / (1.f + 8.42420235e-4f * T + 7.08145163e-7f * T * T);
		const float Pv = (0.317398726f + 4.22806245e-5f * T + 4.20481691e-8f * T * T) / (1.f - 2.89741816e-5f * T + 1.61456053e-7f * T * T);
		const float Ud = (-1.13758118e9f - 1.91615621e6f * T - 1.53177f * T * T) / FMath::Square(1.41213984e6f + 1189.62f * T + T * T);
		const float Vd = (1.97471536e9f - 705674.0f * T - 308.607f * T * T) / FMath::Square(6.19363586e6f - 179.456f * T + T * T);
		const FVector2f Iso = FVector2f(Ud, Vd).GetSafeNormal();
		const float Tint = FVector2f::DotProduct(AdaptUV - FVector2f(Pu, Pv), FVector2f(Iso.Y, -Iso.X)) / 0.05f;
		PP.TemperatureType = ETemperatureMethod::TEMP_WhiteBalance;
		PP.WhiteTemp = T;
		PP.WhiteTint = FMath::Clamp(Tint, -1.f, 1.f);
	}
	SetupWindowCapture(InView, bUnder, Now);

	if (PP.AutoExposureMethod == AEM_Manual) return;
	if (!bUnder)
	{
		// Белая точка над водой — из средней яркости прошлого кадра, как у движка (PostProcessEyeAdaptation).
		const float Average = InView.State ? InView.State->GetLastAverageSceneLuminance() : 0.f;
		if (Average > 0.f)
		{
			AboveWaterEV = FMath::Log2(Average / LuminanceMax) + FMath::Log2(1.f / 0.18f);
			bHasAboveWaterEV = true;
		}
		return;
	}
	if (!bHasAboveWaterEV) return;
	const float MinEV = AboveWaterEV - FMath::Max(CVarSwampWaterEyeAdaptStops.GetValueOnGameThread(), 0.f);
	const float MinBrightness = bExtended ? MinEV : LuminanceMax * FMath::Pow(2.f, MinEV);
	PP.AutoExposureMinBrightness = FMath::Max(PP.AutoExposureMinBrightness, MinBrightness);
	PP.AutoExposureMaxBrightness = FMath::Max(PP.AutoExposureMaxBrightness, PP.AutoExposureMinBrightness);
}

// Куб надводного мира: с поверхности над глазом под водой; переснимается, когда глаз ушёл на 1.5 м или прошло 2 с.
void FSwampWaterViewExtension::SetupWindowCapture(FSceneView& InView, bool bUnder, double Now)
{
	USceneCaptureComponentCube* Capture = WindowCapture.Get();
	UTextureRenderTargetCube* Target = WindowTarget.Get();
	FTextureResource* Texture = nullptr;
	if (bUnder && Capture && Target)
	{
		const FVector Eye = InView.ViewMatrices.GetViewOrigin();
		const FVector At(Eye.X, Eye.Y, WaterLevel + 5.0);
		if (FVector::Dist(At, WindowCapturedAt) > 150.0 || Now - WindowCapturedTime > 2.0)
		{
			Capture->SetWorldLocation(At);
			Capture->CaptureSceneDeferred();
			WindowCapturedAt = At;
			WindowCapturedTime = Now;
		}
		Texture = Target->GameThread_GetRenderTargetResource();
	}
	ENQUEUE_RENDER_COMMAND(SwampWaterWindow)([Self = StaticCastSharedRef<FSwampWaterViewExtension>(AsShared()), Texture](FRHICommandListImmediate&)
	{
		Self->WindowTexture = Texture;
	});
}

// Снимок глубины со стороны солнца: над уровнем воды у глаза, на 50 м к солнцу, смотрит вдоль луча. Центр привязан к
// ячейкам воды — тени не дрожат при движении глаза.
void FSwampWaterViewExtension::SetupSunCapture(FSceneView& InView)
{
	USceneCaptureComponent2D* Capture = SunCapture.Get();
	UTextureRenderTarget2D* Target = SunTarget.Get();
	USwampWaterSubsystem* Water = Owner.Get();
	FSunFrame Frame;
	if (Capture && Target && Water)
	{
		const FVector3f S = Water->GetToSun();
		const FVector ToSun(S.X, S.Z, S.Y);
		if (ToSun.Z > 0.02)
		{
			const FVector Eye = InView.ViewMatrices.GetViewOrigin();
			const double Snap = 19.5;
			const FVector Center(FMath::FloorToDouble(Eye.X / Snap) * Snap, FMath::FloorToDouble(Eye.Y / Snap) * Snap, WaterLevel);
			const FVector Position = Center + ToSun * 5000.0;
			const FRotator Rotation = (-ToSun).Rotation();
			Capture->SetWorldLocationAndRotation(Position, Rotation);
			Capture->CaptureSceneDeferred();
			const FRotationMatrix M(Rotation);
			Frame.Texture = Target->GameThread_GetRenderTargetResource();
			Frame.Position = Position;
			Frame.Forward = FVector3f(M.GetUnitAxis(EAxis::X));
			Frame.Right = FVector3f(M.GetUnitAxis(EAxis::Y));
			Frame.Up = FVector3f(M.GetUnitAxis(EAxis::Z));
			Frame.Width = Capture->OrthoWidth;
		}
	}
	ENQUEUE_RENDER_COMMAND(SwampWaterSunFrame)([Self = StaticCastSharedRef<FSwampWaterViewExtension>(AsShared()), Frame](FRHICommandListImmediate&)
	{
		Self->SunFrame = Frame;
	});
}

void FSwampWaterViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	if (InView.bIsSceneCapture || InView.bIsReflectionCapture || InView.bIsPlanarReflection) return;
	LimitUnderwaterAdaptation(InView);
	SetupSunCapture(InView);
	USceneCaptureComponent2D* Capture = PlanarCapture.Get();
	UTextureRenderTarget2D* Target = PlanarTarget.Get();
	const FVector Eye = InView.ViewMatrices.GetViewOrigin();
	FPlanarFrame Frame;
	if (Capture && Target && CVarSwampWaterPlanar.GetValueOnGameThread() > 0 && Eye.Z > WaterLevel && InView.IsPerspectiveProjection())
	{
		const float Scale = FMath::Clamp(CVarSwampWaterPlanarScale.GetValueOnGameThread(), 10.f, 100.f) * 0.01f;
		const FIntPoint Size = (FVector2D(InView.UnscaledViewRect.Size()) * Scale).IntPoint().ComponentMax(FIntPoint(16, 16));
		if (Target->SizeX != Size.X || Target->SizeY != Size.Y) Target->ResizeTarget(Size.X, Size.Y);

		const FVector MirrorEye(Eye.X, Eye.Y, 2.0 * WaterLevel - Eye.Z);
		const FRotator MirrorRot(-InView.ViewRotation.Pitch, InView.ViewRotation.Yaw, -InView.ViewRotation.Roll);
		// Проекция без джиттера: в SetupView он ещё не добавлен (а ViewToClipNoAA ещё не заполнен — единичная).
		const FMatrix Projection = InView.ViewMatrices.GetViewToClip();
		Capture->SetWorldLocationAndRotation(MirrorEye, MirrorRot);
		Capture->CustomProjectionMatrix = Projection;
		Capture->ClipPlaneBase = FVector(Eye.X, Eye.Y, WaterLevel - 5.0);
		Capture->CaptureSceneDeferred();

		const FVector PreView = InView.ViewMatrices.GetPreViewTranslation();
		const FMatrix ToClip = FTranslationMatrix(-(PreView + MirrorEye)) * FInverseRotationMatrix(MirrorRot)
			* FMatrix(FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1)) * Projection;
		Frame.Texture = Target->GameThread_GetRenderTargetResource();
		Frame.TranslatedToClip = FMatrix44f(ToClip);
		Frame.PreViewTranslation = PreView;
	}
	ENQUEUE_RENDER_COMMAND(SwampWaterPlanar)([Self = StaticCastSharedRef<FSwampWaterViewExtension>(AsShared()), Frame](FRHICommandListImmediate&)
	{
		Self->Planar = Frame;
	});
}

namespace
{
	BEGIN_SHADER_PARAMETER_STRUCT(FWaterDepthCopyParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthIn)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	BEGIN_SHADER_PARAMETER_STRUCT(FWaterDrawParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SimSurface)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SimSurfacePrev)
		SHADER_PARAMETER(float, SimBlend)
		SHADER_PARAMETER(FVector4f, SimParams)
		SHADER_PARAMETER(FVector3f, WorldOriginTranslated)
		SHADER_PARAMETER(float, W_BubbleLife)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorCopy)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthCopy)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, BubbleDist)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER(float, WaterTime)
		SHADER_PARAMETER(FVector3f, Absorption)
		SHADER_PARAMETER(float, Scattering)
		SHADER_PARAMETER(float, Backscatter)
		SHADER_PARAMETER(FVector3f, ToSunSim)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float>, SunVis)
		SHADER_PARAMETER_SAMPLER(SamplerState, SunVisSampler)
		SHADER_PARAMETER(FVector3f, SunVisOrigin)
		SHADER_PARAMETER(FVector3f, SunVisExtent)
		SHADER_PARAMETER(int32, SunVisDebug)
		SHADER_PARAMETER_RDG_TEXTURE(TextureCube, WindowCube)
		SHADER_PARAMETER_SAMPLER(SamplerState, WindowSampler)
		SHADER_PARAMETER(float, WindowValid)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float>, Sediment)
		SHADER_PARAMETER(FVector3f, SedimentOrigin)
		SHADER_PARAMETER(FVector3f, SedimentExtent)
		SHADER_PARAMETER(FVector3f, SedimentAbsorption)
		SHADER_PARAMETER_ARRAY(FVector4f, SpecAbsorption, [2])
		SHADER_PARAMETER_ARRAY(FVector4f, SpecSedimentAbsorption, [2])
		SHADER_PARAMETER_ARRAY(FVector4f, SpectralToRGB, [6])
		SHADER_PARAMETER_ARRAY(FVector4f, RGBToSpectral, [6])
		SHADER_PARAMETER(float, SedimentScattering)
		SHADER_PARAMETER(float, ShoreFade)
		SHADER_PARAMETER(float, SimEdgeFade)
		SHADER_PARAMETER(float, NormalStrength)
		SHADER_PARAMETER(float, RippleScale)
		SHADER_PARAMETER(float, RippleSpeed)
		SHADER_PARAMETER(FVector2f, WindDir)
		SHADER_PARAMETER(float, DetailFadeDistance)
		SHADER_PARAMETER(float, RefractionStrength)
		SHADER_PARAMETER(float, Smoothness)
		SHADER_PARAMETER(float, ReflectionStrength)
		SHADER_PARAMETER(float, SpecularStrength)
		SHADER_PARAMETER(int32, SSRSteps)
		SHADER_PARAMETER(float, SSRMaxDistance)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PlanarReflection)
		SHADER_PARAMETER_SAMPLER(SamplerState, PlanarSampler)
		SHADER_PARAMETER(FMatrix44f, PlanarTranslatedToClip)
		SHADER_PARAMETER(float, PlanarValid)
		SHADER_PARAMETER(int32, PlanarDebug)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, WhiteParticles)
		SHADER_PARAMETER(float, WhiteBack)
		SHADER_PARAMETER(FVector3f, FoamColor)
		SHADER_PARAMETER(float, SprayRadius)
		SHADER_PARAMETER(float, Extinction)
		SHADER_PARAMETER(int32, WhitePass)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, VertexData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, WetTiles)
		RDG_BUFFER_ACCESS(DrawArgs, ERHIAccess::IndirectArgs)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	BEGIN_SHADER_PARAMETER_STRUCT(FWaterParticlesDebugParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FVector3f, WorldOriginTranslated)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FParticle>, DebugParticles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, DebugCount)
		SHADER_PARAMETER(float, DebugCell)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	BEGIN_SHADER_PARAMETER_STRUCT(FWaterSurfaceVertsParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SimSurface)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SimSurfacePrev)
		SHADER_PARAMETER(float, SimBlend)
		SHADER_PARAMETER(FVector4f, SimParams)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, VertexDataOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, WetTilesOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DrawArgsOut)
	END_SHADER_PARAMETER_STRUCT()
}

#define WATER_RENDER_SHADER(ClassName, ParamsType) \
	class ClassName : public FGlobalShader \
	{ \
	public: \
		DECLARE_GLOBAL_SHADER(ClassName); \
		using FParameters = ParamsType; \
		SHADER_USE_PARAMETER_STRUCT(ClassName, FGlobalShader); \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
		{ \
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); \
		} \
	};

WATER_RENDER_SHADER(FWaterFullscreenVS, FWaterDepthCopyParameters)
WATER_RENDER_SHADER(FWaterDepthCopyPS, FWaterDepthCopyParameters)
WATER_RENDER_SHADER(FWaterSurfaceVS, FWaterDrawParameters)
WATER_RENDER_SHADER(FWaterSurfacePS, FWaterDrawParameters)
WATER_RENDER_SHADER(FWaterWhiteVS, FWaterDrawParameters)
WATER_RENDER_SHADER(FWaterWhitePS, FWaterDrawParameters)
WATER_RENDER_SHADER(FWaterBubblePS, FWaterDrawParameters)
WATER_RENDER_SHADER(FWaterFullscreenDrawVS, FWaterDrawParameters)
WATER_RENDER_SHADER(FWaterUnderwaterPS, FWaterDrawParameters)
WATER_RENDER_SHADER(FWaterSurfaceVertsCS, FWaterSurfaceVertsParameters)
WATER_RENDER_SHADER(FWaterParticlesVS, FWaterParticlesDebugParameters)
WATER_RENDER_SHADER(FWaterParticlesPS, FWaterParticlesDebugParameters)

IMPLEMENT_GLOBAL_SHADER(FWaterFullscreenVS, "/Plugin/SwampWater/Private/WaterRender.usf", "FullscreenVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FWaterDepthCopyPS, "/Plugin/SwampWater/Private/WaterRender.usf", "DepthCopyPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FWaterSurfaceVS, "/Plugin/SwampWater/Private/WaterRender.usf", "SurfaceVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FWaterSurfacePS, "/Plugin/SwampWater/Private/WaterRender.usf", "SurfacePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FWaterSurfaceVertsCS, "/Plugin/SwampWater/Private/WaterRender.usf", "SurfaceVertsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FWaterParticlesVS, "/Plugin/SwampWater/Private/WaterRender.usf", "ParticlesVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FWaterParticlesPS, "/Plugin/SwampWater/Private/WaterRender.usf", "ParticlesPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FWaterWhiteVS, "/Plugin/SwampWater/Private/WaterRender.usf", "WhiteVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FWaterWhitePS, "/Plugin/SwampWater/Private/WaterRender.usf", "WhitePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FWaterBubblePS, "/Plugin/SwampWater/Private/WaterRender.usf", "BubblePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FWaterFullscreenDrawVS, "/Plugin/SwampWater/Private/WaterRender.usf", "FullscreenVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FWaterUnderwaterPS, "/Plugin/SwampWater/Private/WaterRender.usf", "UnderwaterPS", SF_Pixel);

FSwampWaterViewExtension::FSwampWaterViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

namespace
{
	// Цвет из sRGB (как Color-свойства материала Unity в линейном пространстве).
	FVector3f Linear(float R, float G, float B)
	{
		const FLinearColor C = FLinearColor::FromSRGBColor(FColor(uint8(R * 255.f + 0.5f), uint8(G * 255.f + 0.5f), uint8(B * 255.f + 0.5f)));
		return FVector3f(C.R, C.G, C.B);
	}

	template<typename TVS, typename TPS, typename TParams>
	void AddDrawPass(FRDGBuilder& GB, FRDGEventName&& Name, TParams* P, const FIntRect& Rect, FRHIBlendState* Blend,
		FRHIDepthStencilState* Depth, uint32 NumPrimitives, FRDGBufferRef IndirectArgs = nullptr)
	{
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<TVS> VS(ShaderMap);
		TShaderMapRef<TPS> PS(ShaderMap);
		GB.AddPass(MoveTemp(Name), P, ERDGPassFlags::Raster, [VS, PS, P, Rect, Blend, Depth, NumPrimitives, IndirectArgs](FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(float(Rect.Min.X), float(Rect.Min.Y), 0.f, float(Rect.Max.X), float(Rect.Max.Y), 1.f);
			FGraphicsPipelineStateInitializer Pso;
			RHICmdList.ApplyCachedRenderTargets(Pso);
			Pso.BlendState = Blend;
			Pso.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			Pso.DepthStencilState = Depth;
			Pso.PrimitiveType = PT_TriangleList;
			Pso.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			Pso.BoundShaderState.VertexShaderRHI = VS.GetVertexShader();
			Pso.BoundShaderState.PixelShaderRHI = PS.GetPixelShader();
			SetGraphicsPipelineState(RHICmdList, Pso, 0);
			SetShaderParameters(RHICmdList, VS, VS.GetVertexShader(), *P);
			SetShaderParameters(RHICmdList, PS, PS.GetPixelShader(), *P);
			if (IndirectArgs) RHICmdList.DrawPrimitiveIndirect(IndirectArgs->GetIndirectRHICallBuffer(), 0);
			else if (NumPrimitives > 0) RHICmdList.DrawPrimitive(0, NumPrimitives, 1);
		});
	}
}

void FSwampWaterViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs)
{
	// Снимки сцены (в т.ч. куб отражений) — без воды.
	if (!Sim.IsValid() || !Sim->HasResources() || !InView.bIsViewInfo || InView.bIsSceneCapture || InView.bIsSceneCaptureCube) return;
	const FSwampWaterSim::FRenderParams& R = Sim->Render;
	if (!R.bDrawSurface && !R.bDrawWhitewater && !R.bDrawParticles) return;
	const FViewInfo& View = static_cast<const FViewInfo&>(InView);
	{
		FScopeLock Lock(&ViewLock);
		LastViewOrigin = View.ViewMatrices.GetViewOrigin();
	}
	const FIntRect Rect = View.ViewRect;
	// Тени солнца в воде вокруг глаза (WaterSun.usf).
	const FVector Eye = View.ViewMatrices.GetViewOrigin();
	FSwampWaterSim::FSunVisibility SunVis, Sediment;
	FRDGTextureRef WindowCubeTex = WindowTexture && WindowTexture->GetTextureRHI()
		? RegisterExternalTexture(GraphBuilder, WindowTexture->GetTextureRHI(), TEXT("SwampWater.WindowCube")) : nullptr;
	{
	RDG_GPU_STAT_SCOPE(GraphBuilder, WaterLight);
	FSwampWaterSim::FSunDepth SunDepth;
	if (SunFrame.Texture && SunFrame.Texture->GetTextureRHI())
	{
		// В оси симуляции (м, y вверх): мир (X, Y, Z)·0.01 → (x, z, y).
		auto ToSimDir = [](const FVector3f& D) { return FVector3f(D.X, D.Z, D.Y); };
		SunDepth.Depth = RegisterExternalTexture(GraphBuilder, SunFrame.Texture->GetTextureRHI(), TEXT("SwampWater.SunDepth"));
		SunDepth.Position = FVector3f(float(SunFrame.Position.X * 0.01), float(SunFrame.Position.Z * 0.01), float(SunFrame.Position.Y * 0.01));
		SunDepth.Forward = ToSimDir(SunFrame.Forward);
		SunDepth.Right = ToSimDir(SunFrame.Right);
		SunDepth.Up = ToSimDir(SunFrame.Up);
		SunDepth.Width = SunFrame.Width * 0.01f;
	}
	Sediment = Sim->UpdateSediment(GraphBuilder, FVector2f(float(Eye.X * 0.01), float(Eye.Y * 0.01)), R.DeltaTime);
	SunVis = Sim->BuildSunVisibility(GraphBuilder, FVector2f(float(Eye.X * 0.01), float(Eye.Y * 0.01)), R.ToSun, SunDepth,
		&Sediment, SwampWaterOptics::SedimentAbsorption.Y + SwampWaterOptics::SedimentScattering);
	}
	const auto SceneTex = Inputs.SceneTextures->GetParameters();
	FRDGTextureRef SceneColor = SceneTex->SceneColorTexture;
	FRDGTextureRef SceneDepth = SceneTex->SceneDepthTexture;
	if (!SceneColor || !SceneDepth) return;

	RDG_EVENT_SCOPE_STAT(GraphBuilder, WaterRender, "SwampWater.Render");

	// Копии цвета (преломление) и глубины сцены (глубина воды пишется в саму глубину сцены). Цвет копируется после
	// пузырей под водой — поверхность видит их сквозь толщу; глубина — до них (кромка берега, отражения).
	FRDGTextureDesc ColorDesc = SceneColor->Desc;
	ColorDesc.Flags = TexCreate_ShaderResource;
	ColorDesc.NumMips = 1;
	FRDGTextureRef ColorCopy = GraphBuilder.CreateTexture(ColorDesc, TEXT("SwampWater.SceneColorCopy"));
	FRDGTextureRef DepthCopy = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(SceneDepth->Desc.Extent, PF_R32_FLOAT,
		FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_RenderTargetable), TEXT("SwampWater.SceneDepthCopy"));
	FRDGTextureRef BubbleDist = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(SceneDepth->Desc.Extent, PF_R32_FLOAT,
		FClearValueBinding(FLinearColor(1e6f, 0.f, 0.f, 0.f)), TexCreate_ShaderResource | TexCreate_RenderTargetable), TEXT("SwampWater.BubbleDist"));
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, WaterCopies, "SwampWater.Copies");
		auto* P = GraphBuilder.AllocParameters<FWaterDepthCopyParameters>();
		P->SceneDepthIn = SceneDepth;
		P->RenderTargets[0] = FRenderTargetBinding(DepthCopy, ERenderTargetLoadAction::ENoAction);
		AddDrawPass<FWaterFullscreenVS, FWaterDepthCopyPS>(GraphBuilder, RDG_EVENT_NAME("SwampWater.DepthCopy"), P, Rect,
			TStaticBlendState<>::GetRHI(), TStaticDepthStencilState<false, CF_Always>::GetRHI(), 1);
	}

	// Плоское отражение — если снято для этого вида (иначе небо и отражение по экрану). Снимок пишется в этом же графе
	// раньше; регистрация в графе даёт порядок и переходы состояний (иначе читается ещё не записанная текстура).
	FRDGTextureRef PlanarTexture = nullptr;
	if (Planar.Texture && Planar.Texture->GetTextureRHI() && Planar.PreViewTranslation.Equals(View.ViewMatrices.GetPreViewTranslation(), 1.0))
	{
		PlanarTexture = RegisterExternalTexture(GraphBuilder, Planar.Texture->GetTextureRHI(), TEXT("SwampWater.PlanarReflection"), ERDGTextureFlags::None);
	}

	auto MakeParams = [&]() -> FWaterDrawParameters*
	{
		auto* P = GraphBuilder.AllocParameters<FWaterDrawParameters>();
		P->View = View.ViewUniformBuffer;
		P->SimSurface = GraphBuilder.RegisterExternalTexture(Sim->GetSurface());
		P->SimSurfacePrev = GraphBuilder.RegisterExternalTexture(Sim->GetSurfacePrevious());
		P->SimBlend = R.SurfaceBlend;
		P->SimParams = FVector4f(Sim->Config.Origin.X, Sim->Config.Origin.Y, Sim->Cell, float(Sim->N));
		P->WorldOriginTranslated = FVector3f(View.ViewMatrices.GetPreViewTranslation());
		P->W_BubbleLife = Sim->Config.BubbleLifetime;
		P->SceneColorCopy = ColorCopy;
		P->SceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->SceneDepthCopy = DepthCopy;
		P->BubbleDist = BubbleDist;
		P->SceneTexturesStruct = Inputs.SceneTextures;
		P->WhitePass = 1;
		P->WaterTime = R.Time;
		P->Absorption = SwampWaterOptics::Absorption;
		P->Scattering = SwampWaterOptics::Scattering;
		P->Backscatter = SwampWaterOptics::Backscatter;
		P->ToSunSim = R.ToSun;
		P->SunVis = SunVis.Volume ? SunVis.Volume : GSystemTextures.GetVolumetricBlackDummy(GraphBuilder);
		P->SunVisSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->SunVisOrigin = SunVis.Origin;
		P->SunVisExtent = SunVis.Volume ? SunVis.CellSize * FVector3f(SunVis.Size) : FVector3f::ZeroVector;
		P->SunVisDebug = CVarSwampWaterSunVisDebug.GetValueOnRenderThread();
		P->WindowCube = WindowCubeTex ? WindowCubeTex : GSystemTextures.GetCubeBlackDummy(GraphBuilder);
		P->WindowSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->WindowValid = WindowCubeTex ? 1.f : 0.f;
		P->Sediment = Sediment.Volume ? Sediment.Volume : GSystemTextures.GetVolumetricBlackDummy(GraphBuilder);
		P->SedimentOrigin = Sediment.Origin;
		P->SedimentExtent = Sediment.Volume ? Sediment.CellSize * FVector3f(Sediment.Size) : FVector3f::ZeroVector;
		P->SedimentAbsorption = SwampWaterOptics::SedimentAbsorption;
		P->SedimentScattering = SwampWaterOptics::SedimentScattering;
		using namespace SwampWaterOptics;
		auto Half = [](const float* V, int32 H) { return FVector4f(V[H * 4], V[H * 4 + 1], V[H * 4 + 2], V[H * 4 + 3]); };
		for (int32 H = 0; H < 2; H++)
		{
			P->SpecAbsorption[H] = Half(SpecAbsorption, H);
			P->SpecSedimentAbsorption[H] = Half(SpecSedimentAbsorption, H);
			for (int32 C = 0; C < 3; C++)
			{
				P->SpectralToRGB[C * 2 + H] = Half(SpecToRGB[C], H);
				P->RGBToSpectral[C * 2 + H] = Half(RGBToSpec[C], H);
			}
		}
		// Материал воды: спокойная поверхность, приглушённое отражение (цвет и мутность — константы в WaterRender.usf).
		P->ShoreFade = 0.12f;
		P->SimEdgeFade = 0.08f;
		P->NormalStrength = 0.12f;
		P->RippleScale = 0.4f;
		P->RippleSpeed = 0.06f;
		P->WindDir = FVector2f(1.f, 0.35f);
		P->DetailFadeDistance = 60.f;
		P->RefractionStrength = 0.02f;
		P->Smoothness = 0.8f;
		P->ReflectionStrength = 0.6f;
		P->SpecularStrength = 0.5f;
		P->SSRSteps = FMath::Clamp(CVarSwampWaterSSR.GetValueOnRenderThread(), 0, 64);
		P->SSRMaxDistance = 5000.f;
		P->PlanarReflection = PlanarTexture ? PlanarTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
		P->PlanarSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->PlanarTranslatedToClip = Planar.TranslatedToClip;
		P->PlanarValid = PlanarTexture ? 1.f : 0.f;
		P->PlanarDebug = CVarSwampWaterPlanarDebug.GetValueOnRenderThread();
		P->WhiteBack = R.WhiteBack;
		P->FoamColor = Linear(0.78f, 0.74f, 0.62f); // болотная пена: гуминовые вещества красят её в бежевый
		P->SprayRadius = 0.015f;
		P->Extinction = 1.28f;
		P->WhiteParticles = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(R.bWhitePrev ? Sim->GetWhitePoolPrevious() : Sim->GetWhitePool()));
		P->RenderTargets[0] = FRenderTargetBinding(SceneColor, ERenderTargetLoadAction::ELoad);
		return P;
	};

	// Пузыри под водой — в сцену до поверхности, их расстояние — в BubbleDist (Min). Нет белой воды — BubbleDist пуст.
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, WaterWhite, "SwampWater.Bubbles");
		auto* P = MakeParams();
		P->WhitePass = 0;
		P->SceneColorCopy = GSystemTextures.GetBlackDummy(GraphBuilder); // ещё не снята; проход её не читает
		P->BubbleDist = GSystemTextures.GetBlackDummy(GraphBuilder);     // сам в неё пишет
		P->RenderTargets[1] = FRenderTargetBinding(BubbleDist, ERenderTargetLoadAction::EClear);
		P->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthRead_StencilNop);
		AddDrawPass<FWaterWhiteVS, FWaterBubblePS>(GraphBuilder, RDG_EVENT_NAME("SwampWater.Bubbles"), P, Rect,
			TStaticBlendState<CW_RGB, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_One,
				CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One>::GetRHI(),
			TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI(), R.bDrawWhitewater ? uint32(Sim->Config.WhitewaterPool) * 2 : 0);
	}
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, WaterCopies, "SwampWater.Copies");
		AddCopyTexturePass(GraphBuilder, SceneColor, ColorCopy);
	}
	// Глаз под водой: сцена сквозь толщу (шейдер сам выходит, если глаз над водой). Поверхность потом рисуется поверх
	// по копии без мути — путь сквозь воду до неё она считает сама.
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, WaterSurface, "SwampWater.Underwater");
		auto* P = MakeParams();
		AddDrawPass<FWaterFullscreenDrawVS, FWaterUnderwaterPS>(GraphBuilder, RDG_EVENT_NAME("SwampWater.Underwater"), P, Rect,
			TStaticBlendState<>::GetRHI(), TStaticDepthStencilState<false, CF_Always>::GetRHI(), 1);
	}

	if (R.bDrawSurface)
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, WaterSurface, "SwampWater.Surface");
		// Вершины и список мокрых плиток.
		const int32 N = Sim->N, NT = N / 8;
		FRDGTextureRef VertexData = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(N, N), PF_A32B32G32R32F,
			FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SwampWater.VertexData"));
		FRDGBufferRef WetTiles = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(4, NT * NT), TEXT("SwampWater.WetTiles"));
		FRDGBufferRef DrawArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc(4, 4), TEXT("SwampWater.DrawArgs"));
		static const uint32 InitArgs[4] = { 8 * 8 * 6, 0, 0, 0 };
		GraphBuilder.QueueBufferUpload(DrawArgs, InitArgs, sizeof(InitArgs));
		{
			auto* V = GraphBuilder.AllocParameters<FWaterSurfaceVertsParameters>();
			V->SimSurface = GraphBuilder.RegisterExternalTexture(Sim->GetSurface());
			V->SimSurfacePrev = GraphBuilder.RegisterExternalTexture(Sim->GetSurfacePrevious());
			V->SimBlend = R.SurfaceBlend;
			V->SimParams = FVector4f(Sim->Config.Origin.X, Sim->Config.Origin.Y, Sim->Cell, float(N));
			V->VertexDataOut = GraphBuilder.CreateUAV(VertexData);
			V->WetTilesOut = GraphBuilder.CreateUAV(WetTiles);
			V->DrawArgsOut = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DrawArgs, PF_R32_UINT));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SwampWater.SurfaceVerts"),
				TShaderMapRef<FWaterSurfaceVertsCS>(GetGlobalShaderMap(GMaxRHIFeatureLevel)), V, FIntVector(NT, NT, 1));
		}
		auto* P = MakeParams();
		P->VertexData = VertexData;
		P->WetTiles = GraphBuilder.CreateSRV(WetTiles);
		P->DrawArgs = DrawArgs;
		P->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilNop);
		AddDrawPass<FWaterSurfaceVS, FWaterSurfacePS>(GraphBuilder, RDG_EVENT_NAME("SwampWater.Surface"), P, Rect,
			TStaticBlendState<>::GetRHI(), TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI(), 0, DrawArgs);
	}
	if (R.bDrawWhitewater)
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, WaterWhite, "SwampWater.Whitewater");
		auto* P = MakeParams();
		P->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthRead_StencilNop);
		AddDrawPass<FWaterWhiteVS, FWaterWhitePS>(GraphBuilder, RDG_EVENT_NAME("SwampWater.Whitewater"), P, Rect,
			TStaticBlendState<CW_RGB, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI(),
			TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI(), uint32(Sim->Config.WhitewaterPool) * 2);
	}
	if (R.bDrawParticles)
	{
		// Отладка: частицы воды точками, цвет — по скорости.
		auto* P = GraphBuilder.AllocParameters<FWaterParticlesDebugParameters>();
		P->View = View.ViewUniformBuffer;
		P->WorldOriginTranslated = FVector3f(View.ViewMatrices.GetPreViewTranslation());
		P->DebugParticles = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Sim->GetParticles()));
		P->DebugCount = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Sim->GetParticleCount()));
		P->DebugCell = Sim->Cell;
		P->RenderTargets[0] = FRenderTargetBinding(SceneColor, ERenderTargetLoadAction::ELoad);
		P->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilNop);
		AddDrawPass<FWaterParticlesVS, FWaterParticlesPS>(GraphBuilder, RDG_EVENT_NAME("SwampWater.Particles"), P, Rect,
			TStaticBlendState<>::GetRHI(), TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI(), uint32(Sim->Capacity) * 2);
	}
}

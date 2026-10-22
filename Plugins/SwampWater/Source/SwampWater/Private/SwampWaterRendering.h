#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class FSwampWaterSim;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class FTextureResource;
class USwampWaterSubsystem;

// Оптика болотной воды по каналам RGB (~610/550/465 нм), 1/м — одна на шейдер воды и адаптацию глаза под водой.
namespace SwampWaterOptics
{
	// Поглощение = чистая вода (Pope & Fry 1997: 0.26/0.06/0.015) + гумус (CDOM, a440 = 12, наклон 0.0175/нм — вода
	// болот и торфяных озёр: 0.61/1.75/7.75) + бурый детрит (a440 = 1, наклон 0.011: 0.15/0.30/0.76). Гумус съедает
	// синий, потом зелёный: свет в толще смещён к 580–650 нм и глубже (Ylöstalo et al. 2014, 127 бореальных озёр).
	inline const FVector3f Absorption(1.03f, 2.11f, 8.52f);
	// Рассеяние взвесью: болото несёт много детрита и ила (10–30 мг/л, b ≈ 0.1–0.3 м²/г) — b ≈ 4/м, прозрачность по
	// диску Секки 0.3–0.5 м. Взвесь крупная — рассеивает почти без цвета; цвет даёт свет, который до неё дошёл.
	inline constexpr float Scattering = 4.f;
	// Обратное рассеяние (у ила ~2% от рассеяния).
	inline constexpr float Backscatter = 0.08f;
	// Взвешенный ил со дна, на 1 г/м³ (мг/л). Рассеяние b* ≈ 0.6 м²/г: у прибрежной взвеси 0.5, у лёгкой органики выше
	// (Babin et al. 2003: 0.5 и 1.0 м²/г при 555 нм — из-за плотности частиц). Поглощение — оценка для органического
	// детрита a*(440) ≈ 0.12 м²/г (у минеральной взвеси ~0.03–0.05; для торфяного ила числа в литературе не нашлось),
	// наклон 0.011/нм (Roesler et al. 1989; Babin et al. 2003: 0.0123) → 0.018/0.036/0.091 по RGB.
	inline const FVector3f SedimentAbsorption(0.018f, 0.036f, 0.091f);
	inline constexpr float SedimentScattering = 0.6f;

	// Спектр: 8 полос по 40 нм, 420…700 нм. Поглощение = вода (Pope & Fry 1997) + гумус a440 = 12, S = 0.0175 +
	// детрит a440 = 1, S = 0.011; ил — детрит a*(440) = 0.12 м²/г, S = 0.011 (оценка, см. выше).
	inline constexpr int32 Bands = 8;
	inline constexpr float SpecAbsorption[Bands] = { 18.2794f, 9.2680f, 4.7365f, 2.4680f, 1.3395f, 0.9278f, 0.7543f, 0.8081f };
	inline constexpr float SpecSedimentAbsorption[Bands] = { 0.1495f, 0.0963f, 0.0620f, 0.0399f, 0.0257f, 0.0166f, 0.0107f, 0.0069f };
	// Спектр (доли D65) → XYZ: функции сложения CIE 1931 2° в полосах × D65, Y(D65) = 1.
	inline constexpr float SpecToXYZ[3][Bands] = {
		{ 0.04403f, 0.12943f, 0.00203f, 0.11499f, 0.33167f, 0.28312f, 0.04984f, 0.00308f },
		{ 0.00131f, 0.02671f, 0.13351f, 0.37776f, 0.31491f, 0.12625f, 0.01844f, 0.00111f },
		{ 0.21149f, 0.74295f, 0.11243f, 0.00804f, 0.00062f, 0.00007f, 0.00000f, 0.00000f } };
	// Спектр → линейный sRGB (XYZ → sRGB, белое D65 → (1, 1, 1)).
	inline constexpr float SpecToRGB[3][Bands] = {
		{ 0.03413f, 0.00771f, -0.24692f, -0.20556f, 0.57232f, 0.70118f, 0.12910f, 0.00803f },
		{ -0.03168f, -0.04484f, 0.25518f, 0.60231f, 0.27154f, -0.03779f, -0.01382f, -0.00091f },
		{ 0.22887f, 0.79801f, 0.09299f, -0.06303f, -0.04575f, -0.01005f, -0.00100f, -0.00006f } };
	// Линейный sRGB → спектр: гладкий неотрицательный базис (наименьшая шероховатость при SpecToRGB·базис = единичная).
	inline constexpr float RGBToSpec[3][Bands] = {
		{ 0.07765f, 0.00447f, 0.f, 0.f, 0.21600f, 1.01681f, 1.17039f, 1.17956f },
		{ 0.f, 0.03542f, 0.73669f, 1.08640f, 0.59881f, 0.08855f, 0.00114f, 0.f },
		{ 1.06816f, 0.91214f, 0.30127f, 0.f, 0.f, 0.03444f, 0.04927f, 0.05021f } };
}

// Отрисовка воды перед постобработкой: поверхность (SwampWater.shader) и белая вода (WaterWhitewater.shader)
// поверх готовой сцены, с записью глубины воды — TAA и туман видят её как часть сцены.
class FSwampWaterViewExtension : public FWorldSceneViewExtension
{
public:
	FSwampWaterViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs) override;

	FVector GetLastViewOrigin() const { FScopeLock Lock(&ViewLock); return LastViewOrigin; }

	// Пишется игровым потоком при создании, дальше читается только на render thread.
	TSharedPtr<FSwampWaterSim, ESPMode::ThreadSafe> Sim;

	// Плоское отражение (игровой поток): камера, цель и уровень плоскости воды, см.
	TWeakObjectPtr<USceneCaptureComponent2D> PlanarCapture;
	TWeakObjectPtr<UTextureRenderTarget2D> PlanarTarget;
	double WaterLevel = 0.0;
	// Подсистема (игровой поток): уровень воды над глазом — для экспозиции под водой.
	TWeakObjectPtr<USwampWaterSubsystem> Owner;
	// Глубина сцены со стороны солнца (игровой поток ставит снимок, render thread его читает).
	TWeakObjectPtr<USceneCaptureComponent2D> SunCapture;
	TWeakObjectPtr<UTextureRenderTarget2D> SunTarget;
	TWeakObjectPtr<class USceneCaptureComponentCube> WindowCapture;
	TWeakObjectPtr<class UTextureRenderTargetCube> WindowTarget;

private:
	void LimitUnderwaterAdaptation(FSceneView& InView);
	// Яркость (EV100 белой точки), к которой экспозиция пришла над водой в последний раз.
	float AboveWaterEV = 0.f;
	bool bHasAboveWaterEV = false;
	// «Белое» глаза под водой (CIE 1960 uv, плавно к цели) и время прошлого кадра для сглаживания.
	const FVector2f WhiteUVD65 = FVector2f(0.19783f, 0.31221f);
	FVector2f AdaptUV = WhiteUVD65;
	double LastAdaptTime = -1.0;
	// Для render thread: снимок отражения этого кадра и переход из translated world кадра в клип камеры отражения.
	struct FPlanarFrame
	{
		FTextureResource* Texture = nullptr;
		FMatrix44f TranslatedToClip = FMatrix44f::Identity;
		FVector PreViewTranslation = FVector::ZeroVector;
	};
	FPlanarFrame Planar;
	// Снимок глубины со стороны солнца этого кадра: место и оси камеры снимка (мир, см), ширина.
	struct FSunFrame
	{
		FTextureResource* Texture = nullptr;
		FVector Position = FVector::ZeroVector;
		FVector3f Forward = FVector3f::ZeroVector, Right = FVector3f::ZeroVector, Up = FVector3f::ZeroVector;
		float Width = 0.f;
	};
	FSunFrame SunFrame;
	// Куб надводного мира (render thread) и где/когда он снят (игровой поток).
	FTextureResource* WindowTexture = nullptr;
	FVector WindowCapturedAt = FVector(1e30);
	double WindowCapturedTime = -1e9;
	void SetupWindowCapture(FSceneView& InView, bool bUnder, double Now);
	void SetupSunCapture(FSceneView& InView);
	mutable FCriticalSection ViewLock;
	FVector LastViewOrigin = FVector::ZeroVector;
};

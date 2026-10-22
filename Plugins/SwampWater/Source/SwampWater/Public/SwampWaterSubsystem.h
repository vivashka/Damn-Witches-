#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SwampWaterSubsystem.generated.h"

class FSwampWaterSim;
class FSwampWaterViewExtension;
class UPrimitiveComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
struct FSwampWaterOp;
struct FSwampWaterObstacle;

// Вода мира (перенос WaterSimulation из Unity): строит сетку над ландшафтом, решает, когда шагать, собирает тела
// (WaterObstacleCollector), считает силы воды на тела (WaterBodyForces) и отдаёт уровень воды в точке.
UCLASS()
class SWAMPWATER_API USwampWaterSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
	virtual bool IsTickableInEditor() const override { return true; }

	// Уровень воды (см, мировой Z) и течение (см/с, мировые XY) в точке по данным прошлого шага. false — воды нет.
	UFUNCTION(BlueprintCallable, Category = "Swamp Water")
	bool SampleWater(const FVector& WorldLocation, float& OutHeight, FVector2D& OutFlow) const;

	// Залить всё заново до стартового уровня.
	UFUNCTION(BlueprintCallable, Category = "Swamp Water")
	void ResetWater();

	bool IsReady() const { return Sim.IsValid(); }

	// Для отрисовки (игровой поток → render thread через view extension).
	TSharedPtr<FSwampWaterSim, ESPMode::ThreadSafe> GetSim() const { return Sim; }
	float GetSurfaceBlend() const;

private:
	struct FShape;
	struct FTracked;

	bool TryInit(float DeltaTime);
	void ReleaseSim();
	bool IsEditorWorld() const;
	uint32 BuiltGeneration = 0;
	float RenderTime = 0.f;
	bool SampleSim(const FVector3f& P, float& OutHeight, FVector2f& OutFlow) const;
	void UpdateWater(float DeltaTime);
	void BeginStep(float StepDt, TArray<FSwampWaterOp>& Ops);
	void RunPart(TArray<FSwampWaterOp>& Ops);
	void EndStep(TArray<FSwampWaterOp>& Ops);
	void CollectObstacles(float DeltaTime, TArray<FSwampWaterObstacle>& Out);
	void ApplyBodyForces(float DeltaTime);
	void FlushOps(TArray<FSwampWaterOp>&& Ops);
	FBox GetRegion() const;
	FShape& GetShape(UPrimitiveComponent* Comp);
	void DebugTick(float DeltaTime);
	FVector2f GetFocus() const;
	void CreatePlanarReflection();

	// Плоское отражение: сцена, снятая зеркальной камерой из-под воды (ведёт её view extension).
	UPROPERTY(Transient) TObjectPtr<USceneCaptureComponent2D> PlanarCapture;
	UPROPERTY(Transient) TObjectPtr<UTextureRenderTarget2D> PlanarTarget;
	// Глубина сцены со стороны солнца над водой у глаза (тени на воде и в ней — от всего, что выше воды).
	UPROPERTY(Transient) TObjectPtr<USceneCaptureComponent2D> SunCapture;
	UPROPERTY(Transient) TObjectPtr<UTextureRenderTarget2D> SunTarget;
	void CreateSunCapture();
	// Надводный мир над глазом под водой (окно Снеллиуса за краем экрана).
	UPROPERTY(Transient) TObjectPtr<class USceneCaptureComponentCube> WindowCapture;
	UPROPERTY(Transient) TObjectPtr<class UTextureRenderTargetCube> WindowTarget;
	void CreateWindowCapture();
	double LastDebugLog = 0.0;
	bool bShotDone = false;
	bool bDropDone = false;
	int32 RecordFrame = 0;
	// Отладка: игрок ходит туда-обратно по мелководью (swampwater.Walk).
	bool bWalkInit = false, bWalkToB = true;
	double LastWalkLog = 0.0;
	FVector WalkA = FVector::ZeroVector, WalkB = FVector::ZeroVector;
	void WalkTick();

	TSharedPtr<FSwampWaterSim, ESPMode::ThreadSafe> Sim;
	TSharedPtr<FSwampWaterViewExtension, ESPMode::ThreadSafe> ViewExtension;

	// Шаг (как в Unity WaterSimulation).
	float StepClock = 0.f;
	float SurfaceAge = 1e3f;
	// Часы белой воды: время последнего её состояния, длины двух последних шагов (пул хранит последнее и прошлое
	// состояния, каждое ещё и со скоростью — назад на шаг) и время показа, которое идёт плавно в отставании от них.
	double WhiteTime = 0.0, WhiteDisplayTime = 0.0;
	float WhiteStepLen = 1.f / 30.f, WhitePrevLen = 1.f / 30.f, WhiteStepDt = 1.f / 30.f;
	void PublishWhite(float StepDt);
	void UpdateWhiteClock(float DeltaTime, bool& bOutPrev, float& OutBack);
	bool bInStep = false;
	int32 StepParts = 0, PartsDone = 0;
	int32 StepIndex = 0;
	bool bCorrectStep = false;
	bool bWakeAll = false;
	uint32 LastForcedFrame = 0;
	double LastUploadTime = 0.0;
	float MaxObstacleSpeed = 0.f;
	int32 PendingSources = 0;
	TArray<FVector4f> BodySources;
	float InitWaitTime = 0.f;
	bool bInitFailed = false;

	// Копия поверхности на CPU (с GPU, с опозданием) — для сил на тела и SampleWater.
	TArray<FVector4f> SurfaceCPU;
	uint32 SurfaceVersion = 0;

	// Тела: прошлые центры (скорость формы) и формы для сил.
	TMap<TWeakObjectPtr<UPrimitiveComponent>, FVector3f> PreviousCenters;
	TMap<TWeakObjectPtr<UPrimitiveComponent>, TSharedPtr<FShape>> Shapes;

	// Солнце (направленный свет атмосферы) — для света и теней под водой.
	TWeakObjectPtr<class UDirectionalLightComponent> Sun;
public:
	FVector3f GetToSun();
private:
};

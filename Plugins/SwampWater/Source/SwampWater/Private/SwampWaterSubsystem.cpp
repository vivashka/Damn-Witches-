#include "SwampWaterSubsystem.h"
#include "SwampWaterSettings.h"
#include "SwampWaterSim.h"
#include "SwampWaterRendering.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/BoxComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "SceneViewExtension.h"
#include "RenderingThread.h"
#include "Engine/OverlapResult.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Engine/TextureRenderTargetCube.h"
#include "UObject/UObjectIterator.h"

static TAutoConsoleVariable<int32> CVarSwampWaterDebug(TEXT("swampwater.Debug"), 0, TEXT("Log water state every 2 s."));
static TAutoConsoleVariable<FString> CVarSwampWaterWhiteDump(TEXT("swampwater.WhiteDump"), TEXT(""),
	TEXT("Save the whitewater pool after every water step to Saved/WhiteDump: 'StartTime Steps' (s)."));
static TAutoConsoleVariable<FString> CVarSwampWaterShotPos(TEXT("swampwater.ShotPos"), TEXT(""), TEXT("Hold the player at 'X Y Z Pitch Yaw [FromTime]' (cm, deg, s); several poses separated by '|'."));
static TAutoConsoleVariable<FString> CVarSwampWaterDrop(TEXT("swampwater.Drop"), TEXT(""), TEXT("Drop physics spheres: 'Count X Y Z Time' (cm, s)."));
static TAutoConsoleVariable<int32> CVarSwampWaterForceWake(TEXT("swampwater.ForceWake"), 0, TEXT("Worst case: wake all water in the radius every step."));
// Пересобрать воду (после правки ландшафта в редакторе): все подсистемы перечитывают дно и заливают заново.
static uint32 GSwampWaterGeneration = 0;
static FAutoConsoleCommand CmdSwampWaterRebuild(TEXT("swampwater.Rebuild"), TEXT("Rebuild water from the current landscape."),
	FConsoleCommandDelegate::CreateLambda([]() { GSwampWaterGeneration++; }));

static TAutoConsoleVariable<FString> CVarSwampWaterWalk(TEXT("swampwater.Walk"), TEXT(""),
	TEXT("Walk the player back and forth in shallow water: 'Pitch Shore' (deg; Shore 1 — across the waterline onto land)."));
static TAutoConsoleVariable<float> CVarSwampWaterShotTime(TEXT("swampwater.ShotTime"), 0.f, TEXT("Take Saved/WaterShot.png at this game time, s."));
static TAutoConsoleVariable<FString> CVarSwampWaterRecord(TEXT("swampwater.Record"), TEXT(""),
	TEXT("Save a screenshot every frame to Saved/WaterRec/f<N>.png: 'StartTime EndTime' (game time, s). Run with -benchmark -fps=30 for a steady video."));

DEFINE_LOG_CATEGORY_STATIC(LogSwampWaterGame, Log, All);

namespace
{
	// Мир UE (см, Z вверх) ↔ симуляция (м, y вверх): x = X, y = Z, z = Y.
	FVector3f ToSim(const FVector& W) { return FVector3f(float(W.X * 0.01), float(W.Z * 0.01), float(W.Y * 0.01)); }
	FVector3f DirToSim(const FVector& V) { return FVector3f(float(V.X * 0.01), float(V.Z * 0.01), float(V.Y * 0.01)); }
	FVector ToWorld(const FVector3f& S) { return FVector(S.X * 100.0, S.Z * 100.0, S.Y * 100.0); }
	FVector AxesToWorld(const FVector3f& S) { return FVector(S.X, S.Z, S.Y); }
	// Поворот в осях симуляции: перестановка Y↔Z — отражение, угол меняет знак.
	FVector4f QuatToSim(const FQuat& Q) { return FVector4f(float(-Q.X), float(-Q.Z), float(-Q.Y), float(Q.W)); }

	constexpr float MaxStepTime = 0.1f;    // не больше за шаг: ниже 10 FPS вода замедляется
	constexpr int32 PartsPerSubstep = 4;
	constexpr int32 CorrectionInterval = 2;
	constexpr float MaxObstacleSpeedClamp = 30.f;
	constexpr int32 ShapeGrid = 4;
	constexpr int32 MaxCachedShapes = 512;
	constexpr float MaxRelativeSpeed = 10.f;
	constexpr int32 MaxSources = 1024;
	constexpr int32 EntrainSamples = 4;
	constexpr double WhiteClockResponse = 0.25; // с: за сколько время показа белой воды выбирает рассогласование // мест на ватерлинии от одного кусочка тела

	// Форма тела для GPU: капсула/сфера или повёрнутая коробка (в осях симуляции).
	FSwampWaterObstacle Capsule(const FVector3f& A, const FVector3f& B, float Radius)
	{
		FSwampWaterObstacle O;
		FMemory::Memzero(O);
		O.ARadius = FVector4f(A.X, A.Y, A.Z, Radius);
		O.BType = FVector4f(B.X, B.Y, B.Z, 0.f);
		O.Rotation = FVector4f(0, 0, 0, 1);
		O.HalfExtents = FVector4f(0, 0, 0, 1);
		return O;
	}

	FSwampWaterObstacle Box(const FVector3f& Center, const FQuat& Rotation, const FVector3f& HalfExtentsSim)
	{
		FSwampWaterObstacle O;
		FMemory::Memzero(O);
		O.ARadius = FVector4f(Center.X, Center.Y, Center.Z, 0.f);
		O.BType = FVector4f(Center.X, Center.Y, Center.Z, 1.f);
		O.Rotation = QuatToSim(Rotation);
		O.HalfExtents = FVector4f(HalfExtentsSim.X, HalfExtentsSim.Y, HalfExtentsSim.Z, 1.f);
		return O;
	}

	// Половины размеров локальной коробки в осях симуляции (оси коробки переставлены как оси мира).
	FVector3f HalfToSim(const FVector& HalfCm) { return FVector3f(float(HalfCm.X * 0.01), float(HalfCm.Z * 0.01), float(HalfCm.Y * 0.01)); }

	FVector4f BoundsXZ(const FSwampWaterObstacle& O)
	{
		FVector2f Min, Max;
		if (O.BType.W < 0.5f)
		{
			const float R = O.ARadius.W;
			Min = FVector2f(FMath::Min(O.ARadius.X, O.BType.X), FMath::Min(O.ARadius.Z, O.BType.Z)) - FVector2f(R, R);
			Max = FVector2f(FMath::Max(O.ARadius.X, O.BType.X), FMath::Max(O.ARadius.Z, O.BType.Z)) + FVector2f(R, R);
		}
		else
		{
			// Горизонтальный размах повёрнутой коробки (в осях симуляции).
			const FQuat4f Q(O.Rotation.X, O.Rotation.Y, O.Rotation.Z, O.Rotation.W);
			const FVector3f Ex = Q.RotateVector(FVector3f(O.HalfExtents.X, 0, 0));
			const FVector3f Ey = Q.RotateVector(FVector3f(0, O.HalfExtents.Y, 0));
			const FVector3f Ez = Q.RotateVector(FVector3f(0, 0, O.HalfExtents.Z));
			const FVector2f Reach(FMath::Abs(Ex.X) + FMath::Abs(Ey.X) + FMath::Abs(Ez.X), FMath::Abs(Ex.Z) + FMath::Abs(Ey.Z) + FMath::Abs(Ez.Z));
			Min = FVector2f(O.ARadius.X, O.ARadius.Z) - Reach;
			Max = FVector2f(O.ARadius.X, O.ARadius.Z) + Reach;
		}
		return FVector4f(Min.X, Min.Y, Max.X, Max.Y);
	}

	// Тело-триггер (только перекрытия) — не вытесняет воду.
	bool IsSolid(const UPrimitiveComponent* Comp)
	{
		if (!Comp || !Comp->IsCollisionEnabled()) return false;
		if (Comp->IsA<ULandscapeHeightfieldCollisionComponent>()) return false;
		// Тело, у которого воду вытесняют его части (персонаж: капсула для мира, сегменты тела для воды).
		if (Comp->ComponentHasTag(TEXT("SwampWaterIgnore"))) return false;
		return Comp->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block || Comp->GetCollisionResponseToChannel(ECC_WorldDynamic) == ECR_Block
			|| Comp->GetCollisionResponseToChannel(ECC_PhysicsBody) == ECR_Block;
	}

	// Физическое тело, которому принадлежит форма: она сама или физическое тело, к которому она привязана (часть
	// тела, которую двигает анимация, — сегмент персонажа). Нет — форма кинематическая.
	UPrimitiveComponent* PhysicsBodyOf(UPrimitiveComponent* Comp)
	{
		if (Comp->IsSimulatingPhysics()) return Comp;
		for (USceneComponent* Parent = Comp->GetAttachParent(); Parent; Parent = Parent->GetAttachParent())
			if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Parent); Prim && Prim->IsSimulatingPhysics()) return Prim;
		return nullptr;
	}

	// Локальная форма компонента (см, в его осях без масштаба): центр, размер, поворот, вид.
	struct FLocalShape
	{
		enum EKind { KBox, KSphere, KCapsule } Kind = KBox;
		FVector Center = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		FVector Half = FVector::ZeroVector; // коробка: половины; сфера: X — радиус; капсула: X — радиус, Z — половина отрезка
	};

	FLocalShape LocalShape(const UPrimitiveComponent* Comp)
	{
		FLocalShape S;
		if (const UCapsuleComponent* Cap = Cast<UCapsuleComponent>(Comp))
		{
			S.Kind = FLocalShape::KCapsule;
			S.Half = FVector(Cap->GetUnscaledCapsuleRadius(), 0, FMath::Max(Cap->GetUnscaledCapsuleHalfHeight() - Cap->GetUnscaledCapsuleRadius(), 0.f));
			return S;
		}
		if (const USphereComponent* Sph = Cast<USphereComponent>(Comp))
		{
			S.Kind = FLocalShape::KSphere;
			S.Half = FVector(Sph->GetUnscaledSphereRadius());
			return S;
		}
		if (const UBoxComponent* B = Cast<UBoxComponent>(Comp))
		{
			S.Half = B->GetUnscaledBoxExtent();
			return S;
		}
		if (const UBodySetup* BS = const_cast<UPrimitiveComponent*>(Comp)->GetBodySetup())
		{
			const FKAggregateGeom& G = BS->AggGeom;
			if (G.GetElementCount() == 1)
			{
				if (G.SphereElems.Num() == 1)
				{
					S.Kind = FLocalShape::KSphere;
					S.Center = G.SphereElems[0].Center;
					S.Half = FVector(G.SphereElems[0].Radius);
					return S;
				}
				if (G.BoxElems.Num() == 1)
				{
					const FKBoxElem& E = G.BoxElems[0];
					S.Center = E.Center;
					S.Rotation = E.Rotation.Quaternion();
					S.Half = FVector(E.X, E.Y, E.Z) * 0.5;
					return S;
				}
				if (G.SphylElems.Num() == 1)
				{
					const FKSphylElem& E = G.SphylElems[0];
					S.Kind = FLocalShape::KCapsule;
					S.Center = E.Center;
					S.Rotation = E.Rotation.Quaternion();
					S.Half = FVector(E.Radius, 0, E.Length * 0.5);
					return S;
				}
			}
		}
		// Прочие формы — локальной габаритной коробкой.
		const FBoxSphereBounds Local = Comp->CalcBounds(FTransform::Identity);
		S.Center = Local.Origin;
		S.Half = Local.BoxExtent;
		return S;
	}
}

// ---------------------------------------------------------------- формы тел для сил (WaterBodyForces)

struct USwampWaterSubsystem::FShape
{
	TArray<FVector> LocalPoints;  // см, в осях компонента без масштаба
	TArray<float> PointVolumes;   // м³
	TArray<float> PointSizes;     // м
	TArray<float> PreviousVolumes;
	TArray<float> VolumeRates;
	// Часть тела, которую двигает анимация (нога в шаге): точки относительно тела и их своя скорость (м/с, в осях
	// тела) — скорость точки = скорость тела + своя.
	TArray<FVector> PreviousRelative;
	TArray<FVector> AnimationVelocity;
	double UpdateTime = -1.0;
};

USwampWaterSubsystem::FShape& USwampWaterSubsystem::GetShape(UPrimitiveComponent* Comp)
{
	if (TSharedPtr<FShape>* Found = Shapes.Find(Comp)) return **Found;
	if (Shapes.Num() > MaxCachedShapes) Shapes.Reset();

	const FLocalShape L = LocalShape(Comp);
	FVector Size = L.Half * 2.0;
	TFunction<bool(const FVector&)> Inside = [](const FVector&) { return true; };
	if (L.Kind == FLocalShape::KSphere)
	{
		const double R = L.Half.X;
		Size = FVector(2 * R);
		Inside = [R](const FVector& P) { return P.SizeSquared() <= R * R; };
	}
	else if (L.Kind == FLocalShape::KCapsule)
	{
		const double R = L.Half.X, H = L.Half.Z;
		Size = FVector(2 * R, 2 * R, 2 * (R + H));
		Inside = [R, H](const FVector& P)
		{
			const double T = FMath::Clamp(P.Z, -H, H);
			return (P - FVector(0, 0, T)).SizeSquared() <= R * R;
		};
	}

	TSharedPtr<FShape> Shape = MakeShared<FShape>();
	const FVector CellSize = Size / ShapeGrid;
	const FVector Scale = Comp->GetComponentTransform().GetScale3D().GetAbs();
	const float Volume = float(FMath::Abs(CellSize.X * CellSize.Y * CellSize.Z * Scale.X * Scale.Y * Scale.Z) * 1e-6);
	const float PointSize = FMath::Max(FMath::Pow(Volume, 1.f / 3.f), 0.01f);
	for (int32 X = 0; X < ShapeGrid; X++)
	for (int32 Y = 0; Y < ShapeGrid; Y++)
	for (int32 Z = 0; Z < ShapeGrid; Z++)
	{
		const FVector Local = -Size * 0.5 + FVector(X + 0.5, Y + 0.5, Z + 0.5) * CellSize; // в осях формы
		if (!Inside(Local)) continue;
		Shape->LocalPoints.Add(L.Center + L.Rotation.RotateVector(Local));
		Shape->PointVolumes.Add(Volume);
		Shape->PointSizes.Add(PointSize);
	}
	Shape->PreviousVolumes.Init(0.f, Shape->LocalPoints.Num());
	Shape->VolumeRates.Init(0.f, Shape->LocalPoints.Num());
	Shape->PreviousRelative.Init(FVector::ZeroVector, Shape->LocalPoints.Num());
	Shape->AnimationVelocity.Init(FVector::ZeroVector, Shape->LocalPoints.Num());
	Shapes.Add(Comp, Shape);
	return *Shape;
}

// ---------------------------------------------------------------- подсистема

bool USwampWaterSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) return false;
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World || !GetDefault<USwampWaterSettings>()->bEnabled) return false;
	return World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Editor;
}

bool USwampWaterSubsystem::IsEditorWorld() const
{
	const UWorld* World = GetWorld();
	return World && World->WorldType == EWorldType::Editor;
}

void USwampWaterSubsystem::ReleaseSim()
{
	ViewExtension.Reset();
	if (PlanarCapture)
	{
		PlanarCapture->DestroyComponent();
		PlanarCapture = nullptr;
	}
	PlanarTarget = nullptr;
	if (SunCapture)
	{
		SunCapture->DestroyComponent();
		SunCapture = nullptr;
	}
	SunTarget = nullptr;
	if (WindowCapture)
	{
		WindowCapture->DestroyComponent();
		WindowCapture = nullptr;
	}
	WindowTarget = nullptr;
	if (Sim.IsValid())
	{
		// Освободить на render thread после всех поставленных команд.
		ENQUEUE_RENDER_COMMAND(SwampWaterRelease)([Ptr = MoveTemp(Sim)](FRHICommandListImmediate&) mutable { Ptr.Reset(); });
	}
	Sim.Reset();
	SurfaceCPU.Reset();
	SurfaceVersion = 0;
	PreviousCenters.Reset();
	Shapes.Reset();
	BodySources.Reset();
	PendingSources = 0;
	bInStep = false;
	InitWaitTime = 0.f;
}

void USwampWaterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void USwampWaterSubsystem::Deinitialize()
{
	ReleaseSim();
	Super::Deinitialize();
}

TStatId USwampWaterSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USwampWaterSubsystem, STATGROUP_Tickables);
}

bool USwampWaterSubsystem::TryInit(float DeltaTime)
{
	UWorld* World = GetWorld();
	const USwampWaterSettings* Settings = GetDefault<USwampWaterSettings>();
	if (!World || bInitFailed) return false;

	// Ландшафт: границы по всем загруженным частям, высоты — по коллизии.
	FBox Bounds(ForceInit);
	ALandscapeProxy* Any = nullptr;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		const FBox B = It->GetComponentsBoundingBox(true);
		if (!B.IsValid) continue;
		Bounds += B;
		Any = *It;
	}
	if (!Any || !Bounds.IsValid) return false;

	FSwampWaterConfig Config;
	const int32 TileSize = FSwampWaterSim::TileSize;
	Config.N = FMath::Max(TileSize, Settings->Resolution / TileSize * TileSize);
	const double SizeCm = Bounds.GetSize().X;
	if (!FMath::IsNearlyEqual(SizeCm, Bounds.GetSize().Y, 1.0))
		UE_LOG(LogSwampWaterGame, Warning, TEXT("Water: landscape is not square, water covers its X width."));
	Config.Cell = float(SizeCm * 0.01 / Config.N);
	Config.Origin = FVector2f(float(Bounds.Min.X * 0.01), float(Bounds.Min.Y * 0.01));
	Config.InitialLevel = Settings->InitialLevel;
	Config.Headroom = Settings->Headroom;
	Config.ShoreReach = Settings->ShoreReach;
	Config.Gravity = Settings->Gravity;
	Config.PressureCycles = Settings->PressureCycles;
	Config.SleepSpeed = Settings->SleepSpeed;
	Config.SimRadius = Settings->SimRadius;
	Config.SeedJitter = Settings->SeedJitter;
	Config.MaxObstacles = Settings->MaxObstacles;
	Config.EntrainSpeed = Settings->EntrainSpeed;
	Config.EntrainRate = Settings->EntrainRate;
	Config.BubbleLifetime = Settings->BubbleLifetime;
	Config.BubbleRise = Settings->BubbleRise;
	Config.WhitewaterPool = Settings->WhitewaterPool;

	// Дно в центрах столбцов.
	const int32 N = Config.N;
	Config.Bed.SetNumUninitialized(N * N);
	int32 Missing = 0;
	for (int32 J = 0; J < N; J++)
	for (int32 I = 0; I < N; I++)
	{
		const FVector P(Bounds.Min.X + (I + 0.5) * Config.Cell * 100.0, Bounds.Min.Y + (J + 0.5) * Config.Cell * 100.0, 0.0);
		const TOptional<float> H = Any->GetHeightAtLocation(P, EHeightfieldSource::Complex);
		if (H.IsSet()) Config.Bed[J * N + I] = H.GetValue() * 0.01f;
		else { Config.Bed[J * N + I] = 1e4f; Missing++; }
	}
	if (Missing > 0)
	{
		InitWaitTime += DeltaTime;
		if (InitWaitTime < 10.f) return false; // части ландшафта ещё грузятся
		UE_LOG(LogSwampWaterGame, Warning, TEXT("Water: %d columns without landscape height (treated as land)."), Missing);
	}

	Sim = MakeShared<FSwampWaterSim, ESPMode::ThreadSafe>(Config);
	if (!Sim->IsValid())
	{
		UE_LOG(LogSwampWaterGame, Warning, TEXT("Water: no columns below the initial level %.2f m."), Config.InitialLevel);
		Sim.Reset();
		bInitFailed = true;
		return false;
	}
	UE_LOG(LogSwampWaterGame, Log, TEXT("Water: landscape %.1f m, origin (%.1f, %.1f) m, bed loaded."), SizeCm * 0.01, Config.Origin.X, Config.Origin.Y);

	ViewExtension = FSceneViewExtensions::NewExtension<FSwampWaterViewExtension>(World);
	ViewExtension->Sim = Sim;
	ViewExtension->Owner = this;
	CreatePlanarReflection();
	CreateSunCapture();
	CreateWindowCapture();
	LastUploadTime = World->GetTimeSeconds();
	ResetWater();
	return true;
}

void USwampWaterSubsystem::ResetWater()
{
	if (!Sim.IsValid()) return;
	bInStep = false; // недоделанный шаг выбрасывается вместе со старой водой
	SurfaceAge = 1e3f;
	StepClock = 0.f;
	WhiteTime = WhiteDisplayTime = 0.0;
	TArray<FSwampWaterOp> Ops;
	FSwampWaterOp& Op = Ops.AddDefaulted_GetRef();
	Op.Type = FSwampWaterOp::Reset;
	FlushOps(MoveTemp(Ops));
	bWakeAll = true; // и первый шаг — всей водой
}

FBox USwampWaterSubsystem::GetRegion() const
{
	const float Size = Sim->Cell * Sim->N, Height = Sim->NY * Sim->Cell;
	const FVector3f Center(Sim->Config.Origin.X + Size * 0.5f, Sim->GridOrigin.Y + Height * 0.5f, Sim->Config.Origin.Y + Size * 0.5f);
	const FVector C = ToWorld(Center);
	const FVector Extent(Size * 50.0, Size * 50.0, (Height + 1.f) * 50.0);
	return FBox(C - Extent, C + Extent);
}

float USwampWaterSubsystem::GetSurfaceBlend() const
{
	return FMath::Clamp(SurfaceAge * GetDefault<USwampWaterSettings>()->StepRate, 0.f, 1.f);
}

// Новое состояние белой воды (конец шага или шаг покоя).
void USwampWaterSubsystem::PublishWhite(float StepDt)
{
	WhitePrevLen = WhiteStepLen;
	WhiteStepLen = StepDt;
	WhiteTime += StepDt;
}

// Белая вода рисуется не на время последнего шага, а на своё время показа — оно идёт ровно, кадр за кадром, примерно
// на шаг позади последнего состояния. Шаги выходят неровно (части шага раскладываются по кадрам; после долгого кадра
// вода догоняет — два шага за кадр или один длинный), и отрисовка, которая вела частицу от прошлого места к новому за
// время шага, на каждом раннем шаге перескакивала вперёд. Запас в шаг (пул хранит и прошлое состояние) это поглощает:
// время показа подстраивает свой ход к отставанию плавно и упирается в края запаса только после долгого простоя.
void USwampWaterSubsystem::UpdateWhiteClock(float DeltaTime, bool& bOutPrev, float& OutBack)
{
	const double Target = WhiteStepLen;              // отставание, к которому тянется время показа
	const double Lag = WhiteTime - WhiteDisplayTime;
	const double Rate = 1.0 + FMath::Clamp((Lag - Target) / WhiteClockResponse, -0.5, 0.5);
	WhiteDisplayTime += DeltaTime * Rate;
	WhiteDisplayTime = FMath::Clamp(WhiteDisplayTime, WhiteTime - WhiteStepLen - WhitePrevLen, WhiteTime);
	const double PrevTime = WhiteTime - WhiteStepLen; // время прошлого состояния
	bOutPrev = WhiteDisplayTime < PrevTime;
	OutBack = float(bOutPrev ? PrevTime - WhiteDisplayTime : WhiteTime - WhiteDisplayTime);
}

void USwampWaterSubsystem::FlushOps(TArray<FSwampWaterOp>&& Ops)
{
	ENQUEUE_RENDER_COMMAND(SwampWaterStep)([Ptr = Sim, Ops = MoveTemp(Ops)](FRHICommandListImmediate& RHICmdList) mutable
	{
		Ptr->Execute_RenderThread(RHICmdList, MoveTemp(Ops));
	});
}

void USwampWaterSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	const USwampWaterSettings* Settings = GetDefault<USwampWaterSettings>();
	const bool bEditor = IsEditorWorld();
	if (bEditor)
	{
		// Пока идёт Play, вода редактора не нужна (и память GPU — тоже).
		bool bPlaying = false;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
			bPlaying |= Context.WorldType == EWorldType::PIE && Context.World() != nullptr;
		if (bPlaying || !Settings->bShowInEditor)
		{
			if (Sim.IsValid()) ReleaseSim();
			return;
		}
	}
	if (Sim.IsValid() && BuiltGeneration != GSwampWaterGeneration) ReleaseSim();
	if (!Sim.IsValid())
	{
		BuiltGeneration = GSwampWaterGeneration;
		bInitFailed = false;
		TryInit(DeltaTime);
		return;
	}

	// Замеры с GPU.
	{
		FSwampWaterFeedback& F = *Sim->Feedback;
		FScopeLock Lock(&F.Lock);
		if (F.SurfaceVersion != SurfaceVersion && F.Surface.Num() == Sim->N * Sim->N)
		{
			Swap(SurfaceCPU, F.Surface);
			SurfaceVersion = F.SurfaceVersion;
		}
	}

	const bool bSimulate = !bEditor || Settings->bSimulateInEditor;
	if (!bEditor) DebugTick(DeltaTime);
	if (!bEditor && SurfaceCPU.Num() > 0 && DeltaTime > 0.f) ApplyBodyForces(DeltaTime);
	UpdateWater(bSimulate ? DeltaTime : 0.f);

	FSwampWaterSim::FRenderParams Params;
	Params.SurfaceBlend = GetSurfaceBlend();
	UpdateWhiteClock(bSimulate ? DeltaTime : 0.f, Params.bWhitePrev, Params.WhiteBack);
	Params.bDrawSurface = Settings->bDrawSurface;
	Params.bDrawWhitewater = Settings->bDrawWhitewater;
	Params.bDrawParticles = Settings->bDrawParticles;
	RenderTime += DeltaTime;
	Params.Time = RenderTime;
	Params.ToSun = GetToSun();
	Params.DeltaTime = bSimulate ? DeltaTime : 0.f;
	// Отладка: пул белой воды после каждого шага — в Saved/WhiteDump; в лог — как отрисовка ведёт её в каждом кадре.
	const FString WhiteDump = CVarSwampWaterWhiteDump.GetValueOnGameThread();
	TArray<FString> DumpArgs;
	WhiteDump.ParseIntoArrayWS(DumpArgs);
	if (DumpArgs.Num() >= 2)
	{
		Params.WhiteDumpStart = FCString::Atof(*DumpArgs[0]);
		Params.WhiteDumpSteps = FCString::Atoi(*DumpArgs[1]);
		UE_LOG(LogSwampWaterGame, Log, TEXT("WhiteFrame %llu dt %.4f white %.4f display %.4f prev %d back %.4f"),
			(unsigned long long)GFrameCounter, DeltaTime, WhiteTime, WhiteDisplayTime, Params.bWhitePrev ? 1 : 0, Params.WhiteBack);
	}
	ENQUEUE_RENDER_COMMAND(SwampWaterRenderParams)([Ptr = Sim, Params](FRHICommandListImmediate&) { Ptr->Render = Params; });
}

// Направление на солнце (оси симуляции): направленный свет, который атмосфера считает солнцем, иначе первый.
FVector3f USwampWaterSubsystem::GetToSun()
{
	if (!Sun.IsValid())
	{
		UDirectionalLightComponent* First = nullptr;
		for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
		{
			if (It->GetWorld() != GetWorld() || !It->IsVisible()) continue;
			if (It->IsUsedAsAtmosphereSunLight()) { Sun = *It; break; }
			if (!First) First = *It;
		}
		if (!Sun.IsValid()) Sun = First;
	}
	if (!Sun.IsValid()) return FVector3f(0.f, 1.f, 0.f);
	const FVector D = -Sun->GetDirection(); // GetDirection — куда идёт свет
	return FVector3f(float(D.X), float(D.Z), float(D.Y)).GetSafeNormal();
}

// Вода шагает по своему времени (StepRate в секунду). Шаг — три части на подшаг; части раскладываются по кадрам,
// пока идёт время шага. Поверхность публикуется в конце шага — рисуется всегда согласованное состояние.
void USwampWaterSubsystem::UpdateWater(float DeltaTime)
{
	TArray<FSwampWaterOp> Ops;
	const USwampWaterSettings* Settings = GetDefault<USwampWaterSettings>();
	if (DeltaTime > 0.f)
	{
		const float StepDt = 1.f / Settings->StepRate;
		SurfaceAge += DeltaTime;
		StepClock = FMath::Min(StepClock + DeltaTime, MaxStepTime);
		while (true)
		{
			if (!bInStep)
			{
				if (StepClock < StepDt) break;
				// Отставание (долгий кадр) шаг забирает целиком, большим dt — подшаги по Куранту держат точность.
				const float Dt = StepClock < 2.f * StepDt ? StepDt : StepClock;
				StepClock -= Dt;
				BeginStep(Dt, Ops);
				continue;
			}
			// Сколько частей должно быть сделано к этому моменту времени шага.
			const int32 Due = (!Settings->bSpreadStepOverFrames || StepClock >= StepDt) ? StepParts
				: FMath::CeilToInt(StepParts * StepClock / StepDt);
			if (PartsDone >= Due) break;
			while (PartsDone < Due) RunPart(Ops);
			if (PartsDone == StepParts) EndStep(Ops);
		}
	}
	FlushOps(MoveTemp(Ops));
}

void USwampWaterSubsystem::BeginStep(float StepDt, TArray<FSwampWaterOp>& Ops)
{
	const USwampWaterSettings* Settings = GetDefault<USwampWaterSettings>();
	const double Now = GetWorld()->GetTimeSeconds();
	int32 ObstacleCount = 0;
	{
		FSwampWaterOp& Op = Ops.AddDefaulted_GetRef();
		Op.Type = FSwampWaterOp::SetObstacles;
		CollectObstacles(float(FMath::Max(Now - LastUploadTime, 1e-4)), Op.Obstacles);
		ObstacleCount = Op.Obstacles.Num();
	}
	// Пузыри от тел, вошедших в воду с прошлого шага: забираются в начале шага, рождаются в конце.
	if (BodySources.Num() > 0)
	{
		PendingSources = FMath::Min(BodySources.Num() / 2, MaxSources);
		FSwampWaterOp& Op = Ops.AddDefaulted_GetRef();
		Op.Type = FSwampWaterOp::SetSources;
		Op.Sources.Append(BodySources.GetData(), PendingSources * 2);
		BodySources.Reset();
	}
	LastUploadTime = Now;

	// Вся вода спит и ничто её не будит — шагать нечего (число плиток приходит с GPU с опозданием, поэтому верим
	// ему, только если оно запрошено позже последнего толчка снаружи).
	if (CVarSwampWaterForceWake.GetValueOnGameThread() != 0) bWakeAll = true;
	if (MaxObstacleSpeed > Settings->SleepSpeed || bWakeAll) LastForcedFrame = GFrameCounter;
	float MeasuredSpeed;
	bool bIdle;
	{
		FSwampWaterFeedback& F = *Sim->Feedback;
		FScopeLock Lock(&F.Lock);
		MeasuredSpeed = F.MeasuredSpeed;
		bIdle = F.bLastFrameValid && F.LastD == 0 && F.LastFrame > LastForcedFrame;
	}
	const float BedFriction = Settings->Gravity * Settings->ManningRoughness * Settings->ManningRoughness;
	const FVector2f Focus = GetFocus();
	if (bIdle)
	{
		FSwampWaterOp& Step = Ops.AddDefaulted_GetRef();
		Step.Type = FSwampWaterOp::SetStep;
		Step.Step0 = FVector4f(StepDt, float(ObstacleCount), 0.f, BedFriction);
		Step.Step1 = FVector4f(0.f, StepDt, Focus.X, Focus.Y);
		if (PendingSources > 0)
		{
			Ops.AddDefaulted_GetRef().Type = FSwampWaterOp::FlushSources;
			PendingSources = 0;
		}
		Ops.AddDefaulted_GetRef().Type = FSwampWaterOp::WhitewaterIdle;
		PublishWhite(StepDt);
		return;
	}
	// Подшаги по условию Куранта: частица за подшаг проходит не больше ячейки.
	const float VMax = FMath::Max(MeasuredSpeed, MaxObstacleSpeed);
	const int32 Sub = FMath::Clamp(FMath::CeilToInt(StepDt * VMax / Sim->Cell), 1, Settings->MaxSubsteps);
	bCorrectStep = StepIndex++ % CorrectionInterval == 0;
	{
		FSwampWaterOp& Step = Ops.AddDefaulted_GetRef();
		Step.Type = FSwampWaterOp::SetStep;
		Step.Step0 = FVector4f(StepDt / Sub, float(ObstacleCount), bWakeAll ? 1.f : 0.f, BedFriction);
		Step.Step1 = FVector4f(bCorrectStep ? 1.f : 0.f, StepDt, Focus.X, Focus.Y);
	}
	WhiteStepDt = StepDt;
	bWakeAll = false;
	Ops.AddDefaulted_GetRef().Type = FSwampWaterOp::StepStart;
	StepParts = Sub * PartsPerSubstep;
	PartsDone = 0;
	bInStep = true;
}

void USwampWaterSubsystem::RunPart(TArray<FSwampWaterOp>& Ops)
{
	FSwampWaterOp& Op = Ops.AddDefaulted_GetRef();
	Op.Type = FSwampWaterOp::Part;
	Op.Index = PartsDone % PartsPerSubstep;
	Op.bFlag = bCorrectStep;
	PartsDone++;
}

void USwampWaterSubsystem::EndStep(TArray<FSwampWaterOp>& Ops)
{
	if (PendingSources > 0)
	{
		Ops.AddDefaulted_GetRef().Type = FSwampWaterOp::FlushSources;
		PendingSources = 0;
	}
	FSwampWaterOp& Op = Ops.AddDefaulted_GetRef();
	Op.Type = FSwampWaterOp::StepEnd;
	Op.Frame = uint32(GFrameCounter);
	PublishWhite(WhiteStepDt);
	bInStep = false;
	SurfaceAge = 0.f;
}

// ---------------------------------------------------------------- тела (WaterObstacleCollector)

void USwampWaterSubsystem::CollectObstacles(float DeltaTime, TArray<FSwampWaterObstacle>& Out)
{
	const int32 MaxCount = GetDefault<USwampWaterSettings>()->MaxObstacles;
	const FBox Region = GetRegion();
	TArray<FOverlapResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SwampWaterObstacles), false);
	GetWorld()->OverlapMultiByObjectType(Hits, Region.GetCenter(), FQuat::Identity,
		FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllDynamicObjects), FCollisionShape::MakeBox(Region.GetExtent()), Params);

	TMap<TWeakObjectPtr<UPrimitiveComponent>, FVector3f> Centers;
	MaxObstacleSpeed = 0.f;
	for (const FOverlapResult& Hit : Hits)
	{
		UPrimitiveComponent* Comp = Hit.GetComponent();
		if (Out.Num() >= MaxCount) break;
		if (!IsSolid(Comp) || Centers.Contains(Comp)) continue;

		const FTransform T = Comp->GetComponentTransform();
		const FLocalShape L = LocalShape(Comp);
		const FVector Scale = T.GetScale3D().GetAbs();
		FSwampWaterObstacle O;
		if (L.Kind == FLocalShape::KCapsule)
		{
			const FVector Axis = T.TransformVectorNoScale(L.Rotation.RotateVector(FVector::UpVector));
			const double Radius = L.Half.X * FMath::Max(Scale.X, Scale.Y);
			const double Half = FMath::Max((L.Half.Z + L.Half.X) * Scale.Z - Radius, 0.0);
			const FVector C = T.TransformPosition(L.Center);
			O = Capsule(ToSim(C - Axis * Half), ToSim(C + Axis * Half), float(Radius * 0.01));
		}
		else if (L.Kind == FLocalShape::KSphere)
		{
			const FVector C = T.TransformPosition(L.Center);
			O = Capsule(ToSim(C), ToSim(C), float(L.Half.X * Scale.GetMax() * 0.01));
		}
		else
		{
			O = Box(ToSim(T.TransformPosition(L.Center)), T.GetRotation() * L.Rotation, HalfToSim(L.Half * Scale));
		}
		const FVector3f Center = (FVector3f(O.ARadius) + FVector3f(O.BType)) * 0.5f;
		Centers.Add(Comp, Center);

		// У физических тел воду тащит честная реакция сопротивления, упрощённое увлечение — только для тел без физики.
		const bool bSimulating = PhysicsBodyOf(Comp) != nullptr;
		O.HalfExtents.W = bSimulating ? 0.f : 1.f;
		// Скорость формы — по её смещению за шаг воды.
		FVector3f Velocity = FVector3f::ZeroVector;
		if (const FVector3f* Prev = PreviousCenters.Find(Comp); Prev && DeltaTime > 0.f) Velocity = (Center - *Prev) / DeltaTime;
		else if (bSimulating) Velocity = DirToSim(Comp->GetPhysicsLinearVelocityAtPoint(ToWorld(Center)));
		Velocity = Velocity.GetClampedToMaxSize(MaxObstacleSpeedClamp);
		O.Velocity = FVector4f(Velocity.X, Velocity.Y, Velocity.Z, 0.f);
		O.BoundsXZ = BoundsXZ(O);
		Out.Add(O);
		MaxObstacleSpeed = FMath::Max(MaxObstacleSpeed, Velocity.Size());
	}
	PreviousCenters = MoveTemp(Centers);
}

// ---------------------------------------------------------------- силы на тела (WaterBodyForces)

bool USwampWaterSubsystem::SampleSim(const FVector3f& P, float& OutHeight, FVector2f& OutFlow) const
{
	OutHeight = 0.f;
	OutFlow = FVector2f::ZeroVector;
	if (!Sim.IsValid() || SurfaceCPU.Num() == 0) return false;
	const int32 N = Sim->N;
	const float Fx = (P.X - Sim->Config.Origin.X) / Sim->Cell - 0.5f;
	const float Fz = (P.Z - Sim->Config.Origin.Y) / Sim->Cell - 0.5f;
	const int32 X0 = FMath::FloorToInt(Fx), Z0 = FMath::FloorToInt(Fz);
	const float Tx = Fx - X0, Tz = Fz - Z0;
	float WeightSum = 0.f;
	for (int32 Dz = 0; Dz <= 1; Dz++)
	for (int32 Dx = 0; Dx <= 1; Dx++)
	{
		const int32 X = FMath::Clamp(X0 + Dx, 0, N - 1), Z = FMath::Clamp(Z0 + Dz, 0, N - 1);
		const FVector4f& C = SurfaceCPU[Z * N + X];
		if (C.Y <= 0.f) continue; // сухо
		const float W = (Dx == 0 ? 1.f - Tx : Tx) * (Dz == 0 ? 1.f - Tz : Tz);
		OutHeight += C.X * W;
		OutFlow += FVector2f(C.Z, C.W) * W;
		WeightSum += W;
	}
	if (WeightSum <= 1e-4f) return false;
	OutHeight /= WeightSum;
	OutFlow /= WeightSum;
	return true;
}

bool USwampWaterSubsystem::SampleWater(const FVector& WorldLocation, float& OutHeight, FVector2D& OutFlow) const
{
	float H;
	FVector2f F;
	if (!SampleSim(ToSim(WorldLocation), H, F)) return false;
	OutHeight = H * 100.f;
	OutFlow = FVector2D(F.X * 100.0, F.Y * 100.0); // сим (x, z) → мир (X, Y)
	return true;
}

// Выталкивающая сила (Архимед), лобовое сопротивление относительно течения, присоединённая масса и удар при входе.
// Форма тела разбивается на маленькие объёмы, сила прикладывается в каждом — крен и качка получаются сами.
void USwampWaterSubsystem::ApplyBodyForces(float DeltaTime)
{
	const USwampWaterSettings* S = GetDefault<USwampWaterSettings>();
	const float Rho = S->WaterDensity, Cd = S->DragCoefficient, Ca = S->AddedMassCoefficient;
	const float EntrainSpeed = S->EntrainSpeed;
	const float BubblesPerVolume = FSwampWaterSim::RestCount / (Sim->Cell * Sim->Cell * Sim->Cell);
	const FVector3f Gravity(0.f, -S->Gravity, 0.f);
	const double Now = GetWorld()->GetTimeSeconds();

	const FBox Region = GetRegion();
	TArray<FOverlapResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SwampWaterBodies), false);
	GetWorld()->OverlapMultiByObjectType(Hits, Region.GetCenter(), FQuat::Identity,
		FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllDynamicObjects), FCollisionShape::MakeBox(Region.GetExtent()), Params);

	struct FPointForce { UPrimitiveComponent* Comp; FVector Position; FVector3f Force; };
	TArray<FPointForce> PointForces;
	TMap<UPrimitiveComponent*, float> Submerged;
	TSet<UPrimitiveComponent*> Seen;

	for (const FOverlapResult& Hit : Hits)
	{
		UPrimitiveComponent* Comp = Hit.GetComponent();
		if (!Comp || !IsSolid(Comp) || Seen.Contains(Comp)) continue;
		UPrimitiveComponent* Body = PhysicsBodyOf(Comp);
		if (!Body) continue;
		Seen.Add(Comp);
		const bool bAnimated = Body != Comp;
		const FTransform BodyT = Body->GetComponentTransform();
		FShape& Shape = GetShape(Comp);
		const FTransform T = Comp->GetComponentTransform();
		const double UpdateDt = Now - Shape.UpdateTime;
		const bool bHasPrevious = Shape.UpdateTime >= 0.0 && UpdateDt > 0.0 && UpdateDt < 0.25;
		Shape.UpdateTime = Now;
		const FVector3f Com = ToSim(Body->GetCenterOfMass());

		for (int32 J = 0; J < Shape.LocalPoints.Num(); J++)
		{
			const FVector PW = T.TransformPosition(Shape.LocalPoints[J]);
			const FVector3f P = ToSim(PW);
			FVector3f PointVelocity = DirToSim(Body->GetPhysicsLinearVelocityAtPoint(PW));
			if (bAnimated)
			{
				const FVector Relative = BodyT.InverseTransformPositionNoScale(PW);
				Shape.AnimationVelocity[J] = bHasPrevious ? (Relative - Shape.PreviousRelative[J]) / UpdateDt : FVector::ZeroVector;
				Shape.PreviousRelative[J] = Relative;
				PointVelocity += DirToSim(BodyT.TransformVectorNoScale(Shape.AnimationVelocity[J]));
			}

			// Доля маленького объёма под водой — плавно, чтобы тело не дёргалось на границе.
			const float Size = Shape.PointSizes[J];
			float Surface = 0.f;
			FVector2f Flow;
			const float Fraction = SampleSim(P, Surface, Flow) ? FMath::Clamp((Surface - (P.Y - Size * 0.5f)) / Size, 0.f, 1.f) : 0.f;
			const float Volume = Shape.PointVolumes[J] * Fraction;
			Shape.VolumeRates[J] = bHasPrevious ? float((Volume - Shape.PreviousVolumes[J]) / UpdateDt) : 0.f;
			Shape.PreviousVolumes[J] = Volume;
			if (Fraction <= 0.f) continue;
			const float VolumeRate = Shape.VolumeRates[J];

			const FVector3f Buoyancy = -Gravity * (Rho * Volume);
			// Площадь, которую кусочек подставляет потоку (кусочки закрывают друг друга — сумма / Grid).
			const float Area = Size * Size * Fraction / ShapeGrid;
			// Течение — невозмущённое, впереди по ходу тела, за пределом возмущения (3 ячейки).
			FVector3f Relative = FVector3f(Flow.X, 0.f, Flow.Y) - PointVelocity;
			const FVector3f Across(Relative.X, 0.f, Relative.Z);
			float AheadH;
			FVector2f Ahead;
			if (Across.SizeSquared() > 1e-6f && SampleSim(P - Across.GetSafeNormal() * (3.f * Sim->Cell), AheadH, Ahead))
				Relative = FVector3f(Ahead.X, 0.f, Ahead.Y) - PointVelocity;
			Relative = Relative.GetClampedToMaxSize(MaxRelativeSpeed);
			const FVector3f Drag = 0.5f * Rho * Cd * Area * Relative.Size() * Relative;
			// Удар при входе: погружаясь, кусочек захватывает dm = Ca·ρ·dV воды и разгоняет её до своей скорости.
			const FVector3f Slam = VolumeRate > 0.f ? Ca * Rho * VolumeRate * Relative : FVector3f::ZeroVector;

			// Вход захватывает воздух (Ervine et al. 1980; Ihmsen et al. 2012): пузыри у края тела.
			if (VolumeRate > 0.f)
			{
				const float Entry = VolumeRate / (Size * Size);
				const float Excess = FMath::Max(Entry - EntrainSpeed, 0.f) / EntrainSpeed;
				const float Bubbles = BubblesPerVolume * VolumeRate * DeltaTime * Excess * Excess;
				if (Bubbles > 0.01f)
				{
					// Воздух входит по всей ватерлинии над пятном кусочка, а не в одной точке напротив его центра: из
					// центров кусочков (сетка ShapeGrid³) пена шла бы лучами по немногим направлениям. Пятна кусочков
					// закрывают сечение тела без щелей; случайные места в пятне, вынесенные от центра тела на его край,
					// закрывают ватерлинию ровно (и только с той стороны, где тело входит).
					const float Plume = 2.6f * FMath::Pow(Entry * Size, 0.7f);
					const float Half = Size * 0.5f / FMath::Sqrt(float(EntrainSamples));
					for (int32 K = 0; K < EntrainSamples; K++)
					{
						const FVector3f Q = P + FVector3f(FMath::FRandRange(-0.5f, 0.5f), 0.f, FMath::FRandRange(-0.5f, 0.5f)) * Size;
						FVector3f Outward = Q - Com;
						Outward.Y = 0.f;
						Outward = Outward.SizeSquared() > 1e-6f ? Outward.GetSafeNormal() : FVector3f(0, 0, 1);
						FVector Closest;
						const FVector Query = ToWorld(FVector3f(Com.X, Surface, Com.Z) + Outward * 3.f);
						if (Comp->GetClosestPointOnCollision(Query, Closest) < 0.f) Closest = Query;
						const FVector3f Edge = ToSim(Closest) + Outward * Half;
						BodySources.Add(FVector4f(Edge.X, Surface, Edge.Z, Half));
						BodySources.Add(FVector4f(PointVelocity.X, Plume, PointVelocity.Z, Bubbles / EntrainSamples));
					}
				}
			}

			PointForces.Add({ Body, PW, Buoyancy + Drag + Slam });
			Submerged.FindOrAdd(Body) += Volume;
		}
	}

	// Присоединённая масса: a = (F_воды + m·g) / (m + m_added) — прикладываем k·F_воды + (k − 1)·m·g, k = m/(m + m_added).
	for (const FPointForce& Pf : PointForces)
	{
		const float M = Pf.Comp->GetMass();
		const float K = M / (M + Ca * Rho * Submerged[Pf.Comp]);
		Pf.Comp->AddForceAtLocation(AxesToWorld(Pf.Force * K) * 100.0, Pf.Position);
	}
	for (const TPair<UPrimitiveComponent*, float>& Pair : Submerged)
	{
		const float M = Pair.Key->GetMass();
		const float K = M / (M + Ca * Rho * Pair.Value);
		Pair.Key->AddForce(AxesToWorld((K - 1.f) * M * Gravity) * 100.0);
	}
}

// Отладка: лог состояния, камера в заданной точке, снимок экрана.
void USwampWaterSubsystem::DebugTick(float DeltaTime)
{
	UWorld* World = GetWorld();
	const double Now = World->GetTimeSeconds();
	if (CVarSwampWaterDebug.GetValueOnGameThread() != 0 && Now - LastDebugLog > 2.0)
	{
		LastDebugLog = Now;
		FSwampWaterFeedback& F = *Sim->Feedback;
		FScopeLock Lock(&F.Lock);
		UE_LOG(LogSwampWaterGame, Log, TEXT("Water: tiles sim %d / with border %d, speed %.3f m/s, surface v%u, inStep %d parts %d/%d, obstacles speed %.2f"),
			F.LastSim, F.LastD, F.MeasuredSpeed, F.SurfaceVersion, bInStep ? 1 : 0, PartsDone, StepParts, MaxObstacleSpeed);
	}
	const FString Pos = CVarSwampWaterShotPos.GetValueOnGameThread();
	if (!Pos.IsEmpty())
	{
		// Несколько поз по времени: 'X Y Z Pitch Yaw [С какого времени]' через '|' — действует последняя наступившая.
		TArray<FString> Poses;
		Pos.ParseIntoArray(Poses, TEXT("|"));
		TArray<FString> P;
		for (const FString& Pose : Poses)
		{
			TArray<FString> Q;
			Pose.ParseIntoArrayWS(Q);
			if (Q.Num() >= 5 && (Q.Num() < 6 || Now >= FCString::Atof(*Q[5]))) P = Q;
		}
		APlayerController* PC = World->GetFirstPlayerController();
		if (P.Num() >= 5 && PC && PC->GetPawn())
		{
			PC->GetPawn()->SetActorLocation(FVector(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2])), false, nullptr, ETeleportType::TeleportPhysics);
			PC->SetControlRotation(FRotator(FCString::Atof(*P[3]), FCString::Atof(*P[4]), 0.f));
		}
	}
	const FString Drop = CVarSwampWaterDrop.GetValueOnGameThread();
	if (!Drop.IsEmpty() && !bDropDone)
	{
		TArray<FString> P;
		Drop.ParseIntoArrayWS(P);
		if (P.Num() >= 5 && Now >= FCString::Atof(*P[4]))
		{
			bDropDone = true;
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
			const int32 Count = FCString::Atoi(*P[0]);
			const FVector Center(FCString::Atod(*P[1]), FCString::Atod(*P[2]), FCString::Atod(*P[3]));
			FRandomStream Rng(7);
			for (int32 I = 0; I < Count; I++)
			{
				const FVector Loc = Center + FVector(Rng.FRandRange(-400, 400), Rng.FRandRange(-400, 400), Rng.FRandRange(0, 300));
				AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>(Loc, FRotator::ZeroRotator);
				if (!A) continue;
				A->SetMobility(EComponentMobility::Movable);
				UStaticMeshComponent* C = A->GetStaticMeshComponent();
				C->SetStaticMesh(Mesh);
				C->SetWorldScale3D(FVector(0.6));
				C->SetCollisionProfileName(TEXT("PhysicsActor"));
				C->SetSimulatePhysics(true);
				C->SetMassOverrideInKg(NAME_None, 40.f, true);
			}
			UE_LOG(LogSwampWaterGame, Log, TEXT("Water: dropped %d spheres"), Count);
		}
	}
	WalkTick();
	const FString Record = CVarSwampWaterRecord.GetValueOnGameThread();
	if (!Record.IsEmpty())
	{
		TArray<FString> P;
		Record.ParseIntoArrayWS(P);
		if (P.Num() >= 2 && Now >= FCString::Atod(*P[0]) && Now <= FCString::Atod(*P[1]))
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("WaterRec") / FString::Printf(TEXT("f%04d.png"), RecordFrame++), false, false);
		}
	}
	const float ShotTime =CVarSwampWaterShotTime.GetValueOnGameThread();
	if (ShotTime > 0.f && Now >= ShotTime && !bShotDone)
	{
		bShotDone = true;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("WaterShot.png"), false, false);
		UE_LOG(LogSwampWaterGame, Log, TEXT("Water: screenshot requested"));
	}
}

// Центр считаемой воды (x, z симуляции): камера игрока.
FVector2f USwampWaterSubsystem::GetFocus() const
{
	// В редакторе игрока нет — центр там, где камера окна (последний нарисованный вид).
	FVector Location = ViewExtension.IsValid() ? ViewExtension->GetLastViewOrigin() : FVector::ZeroVector;
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		FRotator Rotation;
		PC->GetPlayerViewPoint(Location, Rotation);
	}
	const FVector3f P = ToSim(Location);
	return FVector2f(P.X, P.Z);
}

// Камера отражения и её цель. Каждый кадр их ставит view extension (SetupView) по камере кадра; снимок — до сцены.
void USwampWaterSubsystem::CreatePlanarReflection()
{
	PlanarTarget = NewObject<UTextureRenderTarget2D>(this);
	PlanarTarget->RenderTargetFormat = RTF_RGBA16f;
	PlanarTarget->ClearColor = FLinearColor::Black;
	PlanarTarget->InitAutoFormat(64, 64);
	PlanarTarget->UpdateResourceImmediate(true);

	PlanarCapture = NewObject<USceneCaptureComponent2D>(this);
	PlanarCapture->TextureTarget = PlanarTarget;
	PlanarCapture->CaptureSource = SCS_SceneColorHDR;
	PlanarCapture->bCaptureEveryFrame = false;
	PlanarCapture->bCaptureOnMovement = false;
	PlanarCapture->bUseCustomProjectionMatrix = true;
	PlanarCapture->bEnableClipPlane = true; // под водой ничего не отражается (нужен r.AllowGlobalClipPlane)
	PlanarCapture->ClipPlaneNormal = FVector::UpVector;
	PlanarCapture->LODDistanceFactor = 2.f; // в ряби мелочи не видно
	// Флаги — через настройки: при регистрации ShowFlags пересобираются из них.
	TArray<FEngineShowFlagsSetting> Flags;
	for (const TCHAR* Name : { TEXT("LumenGlobalIllumination"), TEXT("LumenReflections"), TEXT("DistanceFieldAO"), TEXT("AmbientOcclusion"),
		TEXT("ScreenSpaceReflections"), TEXT("VolumetricFog"), TEXT("MotionBlur"), TEXT("ContactShadows") })
	{
		FEngineShowFlagsSetting S;
		S.ShowFlagName = Name;
		S.Enabled = false;
		Flags.Add(S);
	}
	PlanarCapture->SetShowFlagSettings(Flags);
	PlanarCapture->RegisterComponentWithWorld(GetWorld());

	ViewExtension->PlanarCapture = PlanarCapture;
	ViewExtension->PlanarTarget = PlanarTarget;
	ViewExtension->WaterLevel = Sim->Config.InitialLevel * 100.0;
}

// Снимок глубины сцены со стороны солнца — ортографический, 25 м вокруг глаза (ставит его view extension по кадру).
// Только глубина: свет, отражения и постобработка не нужны.
void USwampWaterSubsystem::CreateSunCapture()
{
	SunTarget = NewObject<UTextureRenderTarget2D>(this);
	SunTarget->RenderTargetFormat = RTF_R32f;
	SunTarget->ClearColor = FLinearColor(1e9f, 0.f, 0.f, 0.f);
	SunTarget->InitAutoFormat(512, 512);
	SunTarget->UpdateResourceImmediate(true);

	SunCapture = NewObject<USceneCaptureComponent2D>(this);
	SunCapture->TextureTarget = SunTarget;
	SunCapture->CaptureSource = SCS_SceneDepth;
	SunCapture->ProjectionType = ECameraProjectionMode::Orthographic;
	SunCapture->OrthoWidth = 2500.f;
	SunCapture->bCaptureEveryFrame = false;
	SunCapture->bCaptureOnMovement = false;
	TArray<FEngineShowFlagsSetting> Flags;
	for (const TCHAR* Name : { TEXT("Lighting"), TEXT("LumenGlobalIllumination"), TEXT("LumenReflections"), TEXT("DistanceFieldAO"),
		TEXT("AmbientOcclusion"), TEXT("ScreenSpaceReflections"), TEXT("VolumetricFog"), TEXT("Fog"), TEXT("AtmosphericFog"),
		TEXT("PostProcessing"), TEXT("MotionBlur"), TEXT("ContactShadows"), TEXT("DynamicShadows"), TEXT("Translucency"), TEXT("Particles") })
	{
		FEngineShowFlagsSetting S;
		S.ShowFlagName = Name;
		S.Enabled = false;
		Flags.Add(S);
	}
	SunCapture->SetShowFlagSettings(Flags);
	SunCapture->RegisterComponentWithWorld(GetWorld());
	ViewExtension->SunCapture = SunCapture;
	ViewExtension->SunTarget = SunTarget;
}

// Куб надводного мира с поверхности над глазом под водой: всё, что видно в окне Снеллиуса за краем экрана. Мелкий
// (128) — окно сжимает полусферу в 97°, подробностей там немного; снимается редко (view extension).
void USwampWaterSubsystem::CreateWindowCapture()
{
	WindowTarget = NewObject<UTextureRenderTargetCube>(this);
	WindowTarget->Init(128, PF_FloatRGBA);
	WindowTarget->ClearColor = FLinearColor::Black;
	WindowTarget->UpdateResourceImmediate(true);

	WindowCapture = NewObject<USceneCaptureComponentCube>(this);
	WindowCapture->TextureTarget = WindowTarget;
	WindowCapture->CaptureSource = SCS_SceneColorHDR;
	WindowCapture->bCaptureEveryFrame = false;
	WindowCapture->bCaptureOnMovement = false;
	TArray<FEngineShowFlagsSetting> Flags;
	for (const TCHAR* Name : { TEXT("LumenGlobalIllumination"), TEXT("LumenReflections"), TEXT("DistanceFieldAO"), TEXT("AmbientOcclusion"),
		TEXT("ScreenSpaceReflections"), TEXT("VolumetricFog"), TEXT("MotionBlur"), TEXT("ContactShadows") })
	{
		FEngineShowFlagsSetting S;
		S.ShowFlagName = Name;
		S.Enabled = false;
		Flags.Add(S);
	}
	WindowCapture->SetShowFlagSettings(Flags);
	WindowCapture->RegisterComponentWithWorld(GetWorld());
	ViewExtension->WindowCapture = WindowCapture;
	ViewExtension->WindowTarget = WindowTarget;
}

// Отладка: ищет воду глубиной больше 40 см у игрока и водит его по прямой туда-обратно обычным вводом движения:
// вдоль воды (до 6 м) или поперёк кромки на сушу (Shore 1).
void USwampWaterSubsystem::WalkTick()
{
	const FString Walk = CVarSwampWaterWalk.GetValueOnGameThread();
	if (Walk.IsEmpty()) return;
	UWorld* World = GetWorld();
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn) return;
	TArray<FString> P;
	Walk.ParseIntoArrayWS(P);
	const float Pitch = P.Num() > 0 ? FCString::Atof(*P[0]) : -30.f;
	const bool bShore = P.Num() > 1 && FCString::Atoi(*P[1]) != 0;
	const double Level = Sim->Config.InitialLevel * 100.0;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SwampWalk), false, Pawn);
	auto Ground = [&](double X, double Y) -> double
	{
		FHitResult Hit;
		return World->LineTraceSingleByChannel(Hit, FVector(X, Y, Level + 2000.0), FVector(X, Y, Level - 3000.0), ECC_WorldStatic, Params)
			? Hit.ImpactPoint.Z : 1e9;
	};
	if (!bWalkInit)
	{
		bWalkInit = true;
		const FVector O = Pawn->GetActorLocation();
		FVector Start = FVector::ZeroVector;
		bool bFound = false;
		for (int32 R = 0; R <= 60 && !bFound; R++)
			for (int32 K = 0; K < FMath::Max(1, R * 6) && !bFound; K++)
			{
				const double A = 2.0 * PI * K / FMath::Max(1, R * 6);
				const double X = O.X + R * 100.0 * FMath::Cos(A), Y = O.Y + R * 100.0 * FMath::Sin(A);
				if (Ground(X, Y) < Level - 40.0) { Start = FVector(X, Y, 0); bFound = true; }
			}
		if (!bFound) { UE_LOG(LogSwampWaterGame, Warning, TEXT("Walk: no water near the player")); return; }
		// Направление: вдоль воды — самая длинная прямая глубже 15 см; на сушу — самый короткий путь до кромки.
		double Best = bShore ? 1e9 : -1.0;
		FVector BestDir = FVector::ForwardVector;
		for (int32 D = 0; D < 24; D++)
		{
			const FVector Dir(FMath::Cos(2.0 * PI * D / 24), FMath::Sin(2.0 * PI * D / 24), 0);
			double Len = 0;
			while (Len < 1200.0 && Ground(Start.X + Dir.X * (Len + 25.0), Start.Y + Dir.Y * (Len + 25.0)) < Level - 15.0) Len += 25.0;
			if (bShore ? (Len < Best && Len < 1200.0) : Len > Best) { Best = Len; BestDir = Dir; }
		}
		const double Half = Pawn->GetSimpleCollisionHalfHeight();
		WalkA = Start;
		WalkB = Start + BestDir * (bShore ? Best + 200.0 : FMath::Min(Best - 50.0, 600.0));
		WalkA.Z = Ground(WalkA.X, WalkA.Y) + Half + 5.0;
		WalkB.Z = Ground(WalkB.X, WalkB.Y) + Half + 5.0;
		Pawn->SetActorLocation(WalkA, false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogSwampWaterGame, Log, TEXT("Walk: %s -> %s (water %.0f cm)"), *WalkA.ToString(), *WalkB.ToString(), Level - Ground(WalkA.X, WalkA.Y));
		return;
	}
	const FVector Target = bWalkToB ? WalkB : WalkA;
	FVector To = Target - Pawn->GetActorLocation();
	To.Z = 0;
	if (To.Size() < 30.0) { bWalkToB = !bWalkToB; return; }
	const FVector Dir = To.GetSafeNormal();
	Pawn->AddMovementInput(Dir, 1.f);
	PC->SetControlRotation(FRotator(Pitch, Dir.Rotation().Yaw, 0.f));
	const double Now = World->GetTimeSeconds();
	if (Now - LastWalkLog > 0.5)
	{
		LastWalkLog = Now;
		const FVector L = Pawn->GetActorLocation();
		const FVector V = Pawn->GetVelocity();
		float H = 0.f;
		FVector2D Flow;
		const double Bed = Ground(L.X, L.Y);
		const double Depth = SampleWater(L, H, Flow) ? FMath::Max(H - Bed, 0.0) : 0.0;
		UE_LOG(LogSwampWaterGame, Log, TEXT("Walk: speed %.2f m/s, water %.0f cm"), FVector(V.X, V.Y, 0).Size() * 0.01, Depth);
	}
}

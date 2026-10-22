#pragma once

// Вода всей карты — одна трёхмерная модель: несжимаемая жидкость со свободной поверхностью, APIC-частицы на
// MAC-сетке над ландшафтом (перенос WaterSimulation/WaterGrid/WaterParticles/WaterPressure/WaterTiles/
// WaterHeightField/WaterWhitewater из Unity). Живёт на render thread: игровой поток присылает список операций
// кадра (FSwampWaterOps), здесь они превращаются в проходы RDG. Координаты симуляции — метры, ось y вверх
// (x = X/100, y = Z/100, z = Y/100 мира UE).

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIGPUReadback.h"

class FRDGBuilder;
class FWaterCommonParameters;
class FWaterParticlesParameters;

// Тело в воде простой формой для GPU — шесть float4, как struct Obstacle в WaterCommon.ush.
struct FSwampWaterObstacle
{
	FVector4f ARadius;     // xyz — начало оси капсулы / центр коробки, w — радиус
	FVector4f BType;       // xyz — конец оси капсулы, w — тип: 0 капсула/сфера, 1 повёрнутая коробка
	FVector4f Rotation;    // поворот коробки (кватернион, в осях симуляции)
	FVector4f HalfExtents; // xyz — половина размеров коробки, w — dragWeight
	FVector4f Velocity;    // xyz — скорость тела, м/с
	FVector4f BoundsXZ;    // minX, minZ, maxX, maxZ
};
static_assert(sizeof(FSwampWaterObstacle) == 96, "Obstacle layout");

struct FSwampWaterConfig
{
	int32 N = 512;                 // столбцов по стороне
	float Cell = 0.2f;             // м
	FVector2f Origin = FVector2f::ZeroVector; // мировой угол (x, z), м
	TArray<float> Bed;             // N×N: высота дна в центрах столбцов, м
	float InitialLevel = 18.66f;
	float Headroom = 1.f;
	float ShoreReach = 0.3f;
	float Gravity = 9.81f;
	int32 PressureCycles = 2;
	float SleepSpeed = 0.01f;
	float SimRadius = 0.f;     // м, 0 — вся вода
	float SeedJitter = 0.f;
	int32 MaxObstacles = 64;
	// Белая вода.
	float EntrainSpeed = 1.f;
	float EntrainRate = 12.f;
	float BubbleLifetime = 3.f;
	float BubbleRise = 0.15f;
	int32 WhitewaterPool = 1 << 17;
};

// Операции кадра от игрового потока (по порядку).
struct FSwampWaterOp
{
	enum EType : uint8
	{
		Reset,          // залить до стартового уровня
		SetStep,        // параметры шага (StepParams)
		SetObstacles,   // тела на шаг
		SetSources,     // пузыри от тел (WaterBodyForces)
		FlushSources,   // родить пузыри
		StepStart,      // обнулить замер скорости
		Part,           // часть подшага: Index 0–2, bCorrect — поправка плотности на этом шаге
		StepEnd,        // белая вода, поверхность, замеры
		WhitewaterIdle, // вода спит — белая вода всё равно живёт
	};
	EType Type;
	int32 Index = 0;
	bool bFlag = false;
	FVector4f Step0 = FVector4f::Zero(), Step1 = FVector4f::Zero();
	TArray<FSwampWaterObstacle> Obstacles;
	TArray<FVector4f> Sources;
	uint32 Frame = 0;
};

// Замеры с GPU (приходят с опозданием на кадр-другой) — игровому потоку.
struct FSwampWaterFeedback
{
	FCriticalSection Lock;
	float MeasuredSpeed = 0.f;
	int32 LastSim = -1, LastD = -1;
	uint32 LastFrame = 0;
	bool bLastFrameValid = false;
	// Поверхность столбцов (уровень, глубина или −1, скорость x, z) — для сил на тела.
	TArray<FVector4f> Surface;
	uint32 SurfaceVersion = 0;
};

class FSwampWaterSim : public TSharedFromThis<FSwampWaterSim, ESPMode::ThreadSafe>
{
public:
	static constexpr int32 RestCount = 8;
	static constexpr int32 TileSize = 8;
	static constexpr int32 MultigridLevels = 4;

	explicit FSwampWaterSim(const FSwampWaterConfig& InConfig);
	~FSwampWaterSim();

	bool IsValid() const { return Cells > 0; }

	// Render thread.
	void Execute_RenderThread(FRHICommandListImmediate& RHICmdList, TArray<FSwampWaterOp>&& Ops);
	// Поверхность для отрисовки (render thread): последняя и прошлая, могут быть пустыми до первого шага.
	TRefCountPtr<IPooledRenderTarget> GetSurface() const { return Surface[5]; }
	TRefCountPtr<IPooledRenderTarget> GetSurfacePrevious() const { return SurfacePrev; }
	TRefCountPtr<FRDGPooledBuffer> GetWhitePool() const { return WhitePool; }
	TRefCountPtr<FRDGPooledBuffer> GetWhitePoolPrevious() const { return WhitePrev; }

	// Видимость солнца в воде вокруг точки (render thread, в графе вида): объём 1 — свет, 0 — тень.
	struct FSunVisibility
	{
		FRDGTextureRef Volume = nullptr;
		FVector3f Origin = FVector3f::ZeroVector;
		FVector3f CellSize = FVector3f::OneVector;
		FIntVector Size = FIntVector(1, 1, 1);
	};
	// Снимок глубины сцены со стороны солнца (оси симуляции, м): тени от всего, что над водой. Нет — над водой
	// затеняют только рельеф и тела воды.
	struct FSunDepth
	{
		FRDGTextureRef Depth = nullptr;
		FVector3f Position = FVector3f::ZeroVector, Forward = FVector3f::ZeroVector, Right = FVector3f::ZeroVector, Up = FVector3f::ZeroVector;
		float Width = 0.f;
	};
	FSunVisibility BuildSunVisibility(FRDGBuilder& GB, const FVector2f& CenterXZ, const FVector3f& ToSun, const FSunDepth& SunDepth,
		const FSunVisibility* Sediment = nullptr, float SedimentExtinction = 0.f);

	// Ил со дна (г/м³) вокруг точки: шаг переноса раз за кадр (render thread, в графе вида). Объём — как у видимости.
	FSunVisibility UpdateSediment(FRDGBuilder& GB, const FVector2f& CenterXZ, float Dt);
	TRefCountPtr<FRDGPooledBuffer> GetParticles() const { return ParticlesCur; }
	TRefCountPtr<FRDGPooledBuffer> GetParticleCount() const { return PCount; }

	TSharedRef<FSwampWaterFeedback, ESPMode::ThreadSafe> Feedback;

	// Отрисовка (пишет игровой поток через render command, читает view extension).
	struct FRenderParams
	{
		float SurfaceBlend = 1.f;
		// Белая вода рисуется на время показа: из пула прошлого шага (bWhitePrev) или последнего, на WhiteBack с
		// раньше его состояния (частица — на WhiteBack·скорость назад).
		bool bWhitePrev = false;
		float WhiteBack = 0.f;
		bool bDrawSurface = true;
		bool bDrawWhitewater = true;
		bool bDrawParticles = false;
		float Time = 0.f;
		// Отладка: выгружать пул белой воды с этого времени, шагов (swampwater.WhiteDump).
		FVector3f ToSun = FVector3f(0.f, 1.f, 0.f); // к солнцу, оси симуляции
		float DeltaTime = 0.f;                      // шаг кадра игры (ил)
		float WhiteDumpStart = 0.f;
		int32 WhiteDumpSteps = 0;
	};
	FRenderParams Render;
	bool HasResources() const { return bResources; }

	// Геометрия сетки (неизменна).
	FSwampWaterConfig Config;
	int32 N = 0, NY = 0, NT = 0, TileCount = 0, TileCells = 0, Cells = 0, MaxKeys = 0, Capacity = 0, FrozenTotal = 0;
	int32 WhiteRequests = 1 << 15, WhiteSources = 1024;
	float Cell = 0.f;
	FVector3f GridOrigin = FVector3f::ZeroVector;
	double InitialVolume = 0.0;

	struct FLevel { int32 Shift = 0, NY = 0, Block = 0; float H = 0.f; };
	TArray<FLevel> Levels;

private:
	struct FCtx;

	// Неизменные данные для загрузки.
	TArray<int32> ColBaseData, TileMapData, TileBedLayer;
	TArray<FIntPoint> TileCoordData;
	TArray<uint32> TileFrozenCapacity, FrozenStartData;

	// Состояние шага (render thread).
	FVector4f StepData[2] = { FVector4f::Zero(), FVector4f::Zero() };
	TArray<FSwampWaterObstacle> ObstacleData;
	TArray<FVector4f> SourceData;
	bool bResources = false;

	// Сетка.
	TRefCountPtr<FRDGPooledBuffer> ColBase, TileMap, TileCoord, Mom, Wgt, Vel, FaceSolid, ValidA, ValidB, Density, Type, SolidVel,
		Rhs, Correction, RestDensity, Full, BodyFrac, Pressure, Diag, Mask, CellStart, Stats;
	TRefCountPtr<FRDGPooledBuffer> Bed;
	// Плитки.
	TRefCountPtr<FRDGPooledBuffer> TState, TSlot, TWake, TAwake, FrozenStart, FrozenCap, FrozenCount, Lists, Counts, TArgs, Top, PrevTop,
		TBed, Range, SimIndex, Body;
	// Частицы.
	TRefCountPtr<FRDGPooledBuffer> ParticlesCur, ParticlesNext, PCount, NewCount, PArgs, Keys, Speed, Frozen, KeyCount, TileTotal, SlotBase, Extra;
	// Давление: уровни 1..3 (уровень 0 — Type, Pressure, Rhs сетки).
	TRefCountPtr<FRDGPooledBuffer> LType[MultigridLevels], LX[MultigridLevels], LF[MultigridLevels];
	// Поверхность: столбцы, 4 сглаживания, заполнение суши; прошлая.
	TRefCountPtr<IPooledRenderTarget> Surface[6];
	TRefCountPtr<IPooledRenderTarget> SurfacePrev;
	// Белая вода.
	TRefCountPtr<FRDGPooledBuffer> WhitePool, WhitePrev, WhiteFree, WhiteFreeCount, WhiteRequestsBuf;

	// Замеры.
	TUniquePtr<FRHIGPUBufferReadback> SpeedReadback, CountsReadback;
	TUniquePtr<FRHIGPUBufferReadback> SurfaceReadback;
	TRefCountPtr<FRDGPooledBuffer> SurfaceBuf;
	uint32 CountsFrame = 0;
	uint32 SurfaceReadbackCounter = 0;
	bool bSpeedPending = false, bCountsPending = false, bSurfacePending = false;

	// Отладка (swampwater.WhiteDump): пул белой воды после каждого шага — в Saved/WhiteDump.
	struct FWhiteDump { TUniquePtr<FRHIGPUBufferReadback> Readback; int32 Step = -1; float StepDt = 0.f; double Time = 0.0; };
	TArray<FWhiteDump> WhiteDumps;
	// Ил: прошлый кадр (между кадрами — во внешней текстуре) и где был объём.
	TRefCountPtr<IPooledRenderTarget> SedimentPrev;
	FVector3f SedimentPrevOrigin = FVector3f::ZeroVector;
	uint32 SedimentFrame = ~0u;
	FRDGTextureRef SedimentThisFrame = nullptr; // для второго вида того же кадра (тот же граф)
	const FRDGBuilder* SedimentGraph = nullptr;
	int32 WhiteDumpStep = 0;
	void QueueWhiteDump(FCtx& C);

	void CreateResources(FCtx& C);
	void PollReadbacks();

	// Сетка.
	void SetCommon(FCtx& C, FWaterCommonParameters& P, int32 ListSlot);
	void GridGather(FCtx& C);
	void GridClassify(FCtx& C);
	void GridCorrectionRhs(FCtx& C);
	void GridRecordRest(FCtx& C);
	void GridCorrectionFaces(FCtx& C);
	void GridProject(FCtx& C);
	void GridExtrapolate(FCtx& C);
	void GridInitPressure(FCtx& C);
	// Плитки.
	void TilesSchedule(FCtx& C);
	void TilesRetire(FCtx& C, TRefCountPtr<FRDGPooledBuffer>& TypeBuffer, int32 Block, bool bZeroVelocity);
	void TilesUpdateRange(FCtx& C);
	void TilesShiftTop(FCtx& C);
	void Schedule(FCtx& C);
	// Частицы.
	void ParticlesSeed(FCtx& C);
	void ParticlesSort(FCtx& C);
	void ParticlesRecount(FCtx& C);
	void ParticlesClearCounts(FCtx& C);
	void ParticlesPrepareArgs(FCtx& C);
	void ParticlesAdvect(FCtx& C);
	void SetCounting(FCtx& C, FWaterParticlesParameters& P);
	// Давление.
	void PressureSolve(FCtx& C, int32 Cycles, TRefCountPtr<FRDGPooledBuffer>* X);
	// Поверхность.
	void BuildSurface(FCtx& C, bool bAll);
	// Белая вода.
	void WhiteReset(FCtx& C);
	void WhiteUpdate(FCtx& C);
	void WhiteStep(FCtx& C);
	void WhiteEmitSources(FCtx& C);

	void ResetWater(FCtx& C);
	void RunPart(FCtx& C, int32 Part, bool bCorrect);
	void EndStep(FCtx& C, uint32 Frame);
};

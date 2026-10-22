#include "SwampWaterSim.h"
#include "WaterShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "GlobalShader.h"
#include "ShaderParameterUtils.h"
#include "RHIGPUReadback.h"
#include "SystemTextures.h"
#include "Misc/FileHelper.h"
#include "Serialization/MemoryWriter.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogSwampWater, Log, All);
static TAutoConsoleVariable<int32> CVarSwampWaterSedimentDebug(TEXT("swampwater.SedimentDebug"), 0,
	TEXT("1: the silt volume holds the near-bed water speed (mm/s) instead — see it with swampwater.SunVisDebug 2."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarSwampWaterRTLog(TEXT("swampwater.RTLog"), 0, TEXT("Log render-thread cost of the water."), ECVF_RenderThreadSafe);
DECLARE_GPU_STAT(WaterSort);
DECLARE_GPU_STAT(WaterPressure);
DECLARE_GPU_STAT(WaterAdvect);
DECLARE_GPU_STAT(WaterCorrection);
DECLARE_GPU_STAT(WaterStepEnd);
DECLARE_GPU_STAT(WaterReset);

namespace
{
	enum { ListSim = 0, ListD = 1, ListWake = 2, ListRetire = 3 };
	// Аргументы непрямых запусков (по 3 числа): см. ядро TileArgs.
	enum { ArgSimCells = 0, ArgDCells = 1, ArgWake = 2, ArgRetire = 3, ArgLevel1 = 4, ArgLevel2 = 5, ArgLevel3 = 6,
		ArgDTiles = 7, ArgSimTiles = 13, ArgSpread = 14, ArgDThreads = 15, ArgCount = 16 };
	constexpr int32 ParticleStride = 36; // float3 + 6 uint (WaterCommon.ush)
	constexpr int32 ExtrapolationLayers = 3;
	constexpr int32 CorrectionPressureCycles = 1;
	constexpr int32 SmoothSweeps = 2;
	constexpr int32 CoarsestSweeps = 30;
	constexpr int32 MaxNY = 24; // MAX_NY в WaterPressure.usf
	constexpr int32 SolidType = 2;

	int32 Groups(int32 Count) { return FMath::Max(1, (Count + 63) / 64); }
	uint32 ArgOffset(int32 Arg) { return uint32(Arg) * 3u * sizeof(uint32); }

	template<typename T> struct TTag { using Type = T; };

	template<typename TShader>
	TShaderMapRef<TShader> Shader() { return TShaderMapRef<TShader>(GetGlobalShaderMap(GMaxRHIFeatureLevel)); }
}

// ---------------------------------------------------------------- контекст графа

struct FSwampWaterSim::FCtx
{
	FRDGBuilder& GB;
	FSwampWaterSim& S;
	FRDGBufferRef StepParams = nullptr;
	FRDGBufferRef Obstacles = nullptr;

	FCtx(FRDGBuilder& InGB, FSwampWaterSim& InS) : GB(InGB), S(InS) {}

	FRDGBufferRef Buf(const TRefCountPtr<FRDGPooledBuffer>& P) { return GB.RegisterExternalBuffer(P); }
	FRDGBufferSRVRef SRV(const TRefCountPtr<FRDGPooledBuffer>& P) { return GB.CreateSRV(Buf(P)); }
	FRDGBufferUAVRef UAV(const TRefCountPtr<FRDGPooledBuffer>& P) { return GB.CreateUAV(Buf(P)); }
	FRDGBufferUAVRef ArgsUAV(const TRefCountPtr<FRDGPooledBuffer>& P) { return GB.CreateUAV(FRDGBufferUAVDesc(Buf(P), PF_R32_UINT)); }
	FRDGTextureRef Tex(const TRefCountPtr<IPooledRenderTarget>& T) { return GB.RegisterExternalTexture(T); }

	// Параметры шага и тела: меняются между проходами одного графа — у каждой версии свой буфер.
	void InvalidateStep() { StepParams = nullptr; }
	void InvalidateObstacles() { Obstacles = nullptr; }
	FRDGBufferSRVRef StepSRV()
	{
		if (!StepParams)
		{
			StepParams = GB.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 2), TEXT("Water.StepParams"));
			GB.QueueBufferUpload(StepParams, S.StepData, sizeof(S.StepData));
		}
		return GB.CreateSRV(StepParams);
	}
	FRDGBufferSRVRef ObstacleSRV()
	{
		if (!Obstacles)
		{
			const int32 Count = FMath::Max(S.ObstacleData.Num(), 1);
			Obstacles = GB.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FSwampWaterObstacle), Count), TEXT("Water.Obstacles"));
			if (S.ObstacleData.Num() > 0) GB.QueueBufferUpload(Obstacles, S.ObstacleData.GetData(), S.ObstacleData.Num() * sizeof(FSwampWaterObstacle));
			else
			{
				FSwampWaterObstacle Zero;
				FMemory::Memzero(Zero);
				GB.QueueBufferUpload(Obstacles, &Zero, sizeof(Zero));
			}
		}
		return GB.CreateSRV(Obstacles);
	}
};

// ---------------------------------------------------------------- построение сетки (WaterGrid)

FSwampWaterSim::FSwampWaterSim(const FSwampWaterConfig& InConfig)
	: Feedback(MakeShared<FSwampWaterFeedback, ESPMode::ThreadSafe>())
	, Config(InConfig)
{
	N = FMath::Max(TileSize, Config.N / TileSize * TileSize);
	NT = N / TileSize;
	Cell = Config.Cell;
	check(Config.Bed.Num() == N * N);
	const TArray<float>& Heights = Config.Bed;

	const float Reach = Config.InitialLevel + Config.ShoreReach, TopLevel = Config.InitialLevel + Config.Headroom;
	float Lowest = TNumericLimits<float>::Max();
	TileMapData.Init(-1, NT * NT);
	for (int32 k = 0; k < N; k++)
	for (int32 i = 0; i < N; i++)
	{
		const float BedH = Heights[k * N + i];
		if (BedH >= Reach) continue;
		Lowest = FMath::Min(Lowest, BedH);
		InitialVolume += FMath::Max(0.f, Config.InitialLevel - BedH) * Cell * Cell;
		const int32 Ti = (k / TileSize) * NT + i / TileSize;
		if (TileMapData[Ti] >= 0) continue;
		TileMapData[Ti] = TileCoordData.Num();
		TileCoordData.Add(FIntPoint(i / TileSize, k / TileSize));
	}
	TileCount = TileCoordData.Num();
	if (TileCount == 0) return;
	const float Bottom = FMath::FloorToFloat((Lowest - Cell) / Cell) * Cell;
	NY = FMath::CeilToInt((TopLevel - Bottom) / Cell);
	if (NY > MaxNY)
	{
		UE_LOG(LogSwampWater, Error, TEXT("Water: %d cells per column, pressure solver holds at most %d."), NY, MaxNY);
		NY = MaxNY;
	}
	TileCells = TileSize * TileSize * NY;
	Cells = TileCount * TileCells;
	MaxKeys = TileCount * (TileCells + 1);
	GridOrigin = FVector3f(Config.Origin.X, Bottom, Config.Origin.Y);

	// Первая ячейка столбца и вместимость плиток для отложенных частиц.
	ColBaseData.SetNumUninitialized(N * N);
	TileFrozenCapacity.Init(0, TileCount);
	TileBedLayer.Init(NY, TileCount);
	const float PerHeight = RestCount / Cell;
	for (int32 k = 0; k < N; k++)
	for (int32 i = 0; i < N; i++)
	{
		const float BedH = Heights[k * N + i];
		const int32 T = TileMapData[(k / TileSize) * NT + i / TileSize];
		if (BedH >= Reach || T < 0) { ColBaseData[k * N + i] = -1; continue; }
		ColBaseData[k * N + i] = T * TileCells + (k % TileSize) * TileSize + i % TileSize;
		TileFrozenCapacity[T] += uint32(FMath::CeilToInt((Reach - FMath::Max(BedH, Bottom)) * PerHeight)) + RestCount;
		TileBedLayer[T] = FMath::Min(TileBedLayer[T], FMath::Max(0, FMath::FloorToInt((BedH - Bottom) / Cell) - 1));
	}
	for (int32 T = 0; T < TileCount; T++) TileFrozenCapacity[T] += 128;
	// Нижний слой плитки — не выше, чем у соседей (частица соседа у края отдаёт вес граням этой плитки).
	const TArray<int32> OwnBed = TileBedLayer;
	for (int32 T = 0; T < TileCount; T++)
	{
		const FIntPoint Tc = TileCoordData[T];
		for (int32 Dz = -1; Dz <= 1; Dz++)
		for (int32 Dx = -1; Dx <= 1; Dx++)
		{
			const int32 X = Tc.X + Dx, Z = Tc.Y + Dz;
			if (X < 0 || Z < 0 || X >= NT || Z >= NT) continue;
			const int32 Nb = TileMapData[Z * NT + X];
			if (Nb >= 0) TileBedLayer[T] = FMath::Min(TileBedLayer[T], OwnBed[Nb]);
		}
	}

	// Уровни мультигрида: каждый вдвое крупнее по всем осям (плитка 8 → 4 → 2 → 1 столбец).
	int32 Ny = NY;
	float H = Cell;
	for (int32 L = 0; L < MultigridLevels; L++)
	{
		const int32 Side = TileSize >> L;
		Levels.Add({ 3 - L, Ny, Side * Side * Ny, H });
		Ny = (Ny + 1) / 2;
		H *= FMath::Sqrt(2.f);
	}

	// Отложенные частицы: у каждой плитки своё место.
	FrozenStartData.SetNumUninitialized(TileCount);
	uint32 Total = 0;
	for (int32 T = 0; T < TileCount; T++)
	{
		FrozenStartData[T] = Total;
		Total += TileFrozenCapacity[T];
	}
	FrozenTotal = int32(Total);

	const float ParticleVolume = Cell * Cell * Cell / RestCount;
	Capacity = FMath::Max(1024, int32(InitialVolume / ParticleVolume * 1.3));
	if (TileCells + 1 > 2048) UE_LOG(LogSwampWater, Error, TEXT("Water: tile has more cells than the group prefix sum holds (2048)."));

	UE_LOG(LogSwampWater, Log, TEXT("Water grid %dx%dx%d, cell %.3f m, tiles %d/%d, cells %d, particles %d, frozen %d"),
		N, NY, N, Cell, TileCount, NT * NT, Cells, Capacity, FrozenTotal);
}

FSwampWaterSim::~FSwampWaterSim() = default;

// ---------------------------------------------------------------- ресурсы

namespace
{
	TRefCountPtr<FRDGPooledBuffer> MakeBuffer(FRDGBuilder& GB, const TCHAR* Name, uint32 Stride, uint32 Num, const void* Data = nullptr, uint32 Clear = 0)
	{
		FRDGBufferRef B = GB.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(Stride, FMath::Max(Num, 1u)), Name);
		if (Data) GB.QueueBufferUpload(B, Data, uint64(Stride) * Num);
		else AddClearUAVPass(GB, GB.CreateUAV(B), Clear);
		return GB.ConvertToExternalBuffer(B);
	}

	TRefCountPtr<FRDGPooledBuffer> MakeArgs(FRDGBuilder& GB, const TCHAR* Name, uint32 Num)
	{
		FRDGBufferRef B = GB.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc(sizeof(uint32), Num), Name);
		TArray<uint32> Init;
		Init.Init(1, Num);
		GB.QueueBufferUpload(B, Init.GetData(), Init.Num() * sizeof(uint32));
		return GB.ConvertToExternalBuffer(B);
	}

	TRefCountPtr<IPooledRenderTarget> MakeSurfaceTexture(FRDGBuilder& GB, const TCHAR* Name, int32 N)
	{
		FRDGTextureRef T = GB.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(N, N), PF_A32B32G32R32F, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV), Name);
		AddClearUAVPass(GB, GB.CreateUAV(T), FVector4f(0, 0, 0, 0));
		return GB.ConvertToExternalTexture(T);
	}
}

void FSwampWaterSim::CreateResources(FCtx& C)
{
	FRDGBuilder& GB = C.GB;
	const uint32 Faces = uint32(Cells) * 3;

	// Сетка.
	Bed = MakeBuffer(GB, TEXT("Water.Bed"), 4, N * N, Config.Bed.GetData());
	ColBase = MakeBuffer(GB, TEXT("Water.ColBase"), 4, N * N, ColBaseData.GetData());
	TileMap = MakeBuffer(GB, TEXT("Water.TileMap"), 4, NT * NT, TileMapData.GetData());
	TileCoord = MakeBuffer(GB, TEXT("Water.TileCoord"), 8, TileCount, TileCoordData.GetData());
	Mom = MakeBuffer(GB, TEXT("Water.Mom"), 4, Faces);
	Wgt = MakeBuffer(GB, TEXT("Water.Wgt"), 4, Faces);
	Vel = MakeBuffer(GB, TEXT("Water.Vel"), 4, Faces);
	FaceSolid = MakeBuffer(GB, TEXT("Water.FaceSolid"), 4, Faces);
	ValidA = MakeBuffer(GB, TEXT("Water.ValidA"), 4, Faces);
	ValidB = MakeBuffer(GB, TEXT("Water.ValidB"), 4, Faces);
	Density = MakeBuffer(GB, TEXT("Water.Density"), 4, Cells);
	Type = MakeBuffer(GB, TEXT("Water.Type"), 4, Cells, nullptr, SolidType);
	SolidVel = MakeBuffer(GB, TEXT("Water.SolidVel"), 16, Cells);
	Rhs = MakeBuffer(GB, TEXT("Water.Rhs"), 4, Cells);
	Correction = MakeBuffer(GB, TEXT("Water.Correction"), 4, Cells);
	RestDensity = MakeBuffer(GB, TEXT("Water.RestDensity"), 4, Cells, nullptr, 0x3f800000u);
	Full = MakeBuffer(GB, TEXT("Water.Full"), 4, Cells, nullptr, 0x3f800000u);
	BodyFrac = MakeBuffer(GB, TEXT("Water.BodyFrac"), 4, Cells);
	Pressure = MakeBuffer(GB, TEXT("Water.Pressure"), 4, Cells);
	Diag = MakeBuffer(GB, TEXT("Water.Diag"), 4, Cells);
	Mask = MakeBuffer(GB, TEXT("Water.Mask"), 4, Cells);
	CellStart = MakeBuffer(GB, TEXT("Water.CellStart"), 4, MaxKeys);
	Stats = MakeBuffer(GB, TEXT("Water.Stats"), 4, 4);

	// Плитки (WaterTiles.Reset: все плитки — как считавшиеся, отложенных частиц нет).
	{
		TArray<int32> Minus, Awake2, TopInit, Zeros;
		Minus.Init(-1, TileCount);
		Awake2.Init(2, TileCount);
		TopInit.Init(NY - 1, TileCount);
		Zeros.Init(0, TileCount);
		TArray<FIntPoint> RangeInit;
		RangeInit.SetNumUninitialized(TileCount);
		for (int32 T = 0; T < TileCount; T++) RangeInit[T] = FIntPoint(TileBedLayer[T], NY - 1);
		TState = MakeBuffer(GB, TEXT("Water.TileState"), 4, TileCount, Awake2.GetData());
		TSlot = MakeBuffer(GB, TEXT("Water.TileSlot"), 4, TileCount, Minus.GetData());
		TWake = MakeBuffer(GB, TEXT("Water.TileWake"), 4, TileCount);
		TAwake = MakeBuffer(GB, TEXT("Water.TileAwake"), 4, TileCount);
		Body = MakeBuffer(GB, TEXT("Water.TileBody"), 4, TileCount);
		FrozenCount = MakeBuffer(GB, TEXT("Water.FrozenCount"), 4, TileCount);
		FrozenStart = MakeBuffer(GB, TEXT("Water.FrozenStart"), 4, TileCount, FrozenStartData.GetData());
		FrozenCap = MakeBuffer(GB, TEXT("Water.FrozenCap"), 4, TileCount, TileFrozenCapacity.GetData());
		Lists = MakeBuffer(GB, TEXT("Water.Lists"), 4, TileCount * 4);
		Counts = MakeBuffer(GB, TEXT("Water.Counts"), 4, 4);
		TArgs = MakeArgs(GB, TEXT("Water.TileArgs"), ArgCount * 3);
		Top = MakeBuffer(GB, TEXT("Water.TileTop"), 4, TileCount, Minus.GetData());
		PrevTop = MakeBuffer(GB, TEXT("Water.TilePrevTop"), 4, TileCount, TopInit.GetData());
		TBed = MakeBuffer(GB, TEXT("Water.TileBed"), 4, TileCount, TileBedLayer.GetData());
		Range = MakeBuffer(GB, TEXT("Water.TileRange"), 8, TileCount, RangeInit.GetData());
		SimIndex = MakeBuffer(GB, TEXT("Water.SimIndex"), 4, TileCount);
	}

	// Частицы.
	ParticlesCur = MakeBuffer(GB, TEXT("Water.ParticlesA"), ParticleStride, Capacity);
	ParticlesNext = MakeBuffer(GB, TEXT("Water.ParticlesB"), ParticleStride, Capacity);
	PCount = MakeBuffer(GB, TEXT("Water.ParticleCount"), 4, 1);
	NewCount = MakeBuffer(GB, TEXT("Water.NewCount"), 4, 1);
	PArgs = MakeArgs(GB, TEXT("Water.ParticleArgs"), 6);
	Keys = MakeBuffer(GB, TEXT("Water.Keys"), 8, Capacity);
	Speed = MakeBuffer(GB, TEXT("Water.Speed"), 4, 2);
	Frozen = MakeBuffer(GB, TEXT("Water.Frozen"), ParticleStride, FMath::Max(FrozenTotal, 1));
	KeyCount = MakeBuffer(GB, TEXT("Water.KeyCount"), 4, MaxKeys);
	TileTotal = MakeBuffer(GB, TEXT("Water.TileTotal"), 4, TileCount);
	SlotBase = MakeBuffer(GB, TEXT("Water.SlotBase"), 4, TileCount);
	Extra = MakeBuffer(GB, TEXT("Water.Extra"), 4, 4);

	// Давление: уровень 0 — буферы сетки; крупные — свои (несчитаемые плитки — стенка).
	for (int32 L = 1; L < Levels.Num(); L++)
	{
		const int32 Count = TileCount * Levels[L].Block;
		LType[L] = MakeBuffer(GB, TEXT("Water.LType"), 4, Count, nullptr, SolidType);
		LX[L] = MakeBuffer(GB, TEXT("Water.LX"), 4, Count);
		LF[L] = MakeBuffer(GB, TEXT("Water.LF"), 4, Count);
	}

	// Поверхность.
	for (int32 P = 0; P < 6; P++) Surface[P] = MakeSurfaceTexture(GB, TEXT("Water.Surface"), N);
	SurfacePrev = MakeSurfaceTexture(GB, TEXT("Water.SurfacePrev"), N);

	// Белая вода.
	WhitePool = MakeBuffer(GB, TEXT("Water.WhitePool"), 16, Config.WhitewaterPool * 2);
	WhitePrev = MakeBuffer(GB, TEXT("Water.WhitePoolPrev"), 16, Config.WhitewaterPool * 2);
	WhiteFree = MakeBuffer(GB, TEXT("Water.WhiteFree"), 4, Config.WhitewaterPool);
	WhiteFreeCount = MakeBuffer(GB, TEXT("Water.WhiteFreeCount"), 4, 1);
	WhiteRequestsBuf = MakeBuffer(GB, TEXT("Water.WhiteRequests"), 16, WhiteRequests * 2);

	SpeedReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Water.SpeedReadback"));
	CountsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Water.CountsReadback"));
	SurfaceReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Water.SurfaceReadback"));
	SurfaceBuf = MakeBuffer(GB, TEXT("Water.SurfaceBuf"), 16, N * N);
	bResources = true;
}

// ---------------------------------------------------------------- общие параметры

void FSwampWaterSim::SetCommon(FCtx& C, FWaterCommonParameters& P, int32 ListSlot)
{
	P.W_N = N;
	P.W_NY = NY;
	P.W_NT = NT;
	P.W_TileCells = TileCells;
	P.W_TileCount = TileCount;
	P.W_GridOrigin = GridOrigin;
	P.W_Cell = Cell;
	P.W_Gravity = Config.Gravity;
	P.W_StepParams = C.StepSRV();
	P.W_ColBase = C.SRV(ColBase);
	P.W_TileMap = C.SRV(TileMap);
	P.W_TileCoord = C.SRV(TileCoord);
	P.W_Bed = C.SRV(Bed);
	P.W_Obstacles = C.ObstacleSRV();
	P.W_SolidVel = C.UAV(SolidVel);
	P.W_Tiles = C.SRV(Lists);
	P.W_TileCounts = C.SRV(Counts);
	P.W_ListSlot = FMath::Max(ListSlot, 0);
	P.W_TileRange = C.SRV(Range);
	P.W_TileBodyIn = C.SRV(Body);
	P.W_FullIn = C.SRV(Full);
	P.IndirectArgs = nullptr;
}

namespace
{
	// Запуск ядра: по аргументам плиток (Arg >= 0) или прямой (Groups).
	template<typename TShader>
	void Run(FRDGBuilder& GB, const TCHAR* Name, typename TShader::FParameters* P, FRDGBufferRef Args, int32 Arg, int32 DirectGroups = 0)
	{
		TShaderMapRef<TShader> Cs = Shader<TShader>();
		if (Args)
		{
			P->Common.IndirectArgs = Args;
			FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("%s", Name), Cs, P, Args, ArgOffset(Arg));
		}
		else
		{
			FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("%s", Name), Cs, P, FIntVector(FMath::Max(DirectGroups, 1), 1, 1));
		}
	}
}

// ---------------------------------------------------------------- плитки (WaterTiles)

void FSwampWaterSim::TilesSchedule(FCtx& C)
{
	auto Bind = [&](FWaterTilesParameters* P)
	{
		SetCommon(C, P->Common, 0);
		P->W_SleepSpeed = Config.SleepSpeed;
		P->W_SimRadius = Config.SimRadius;
		P->W_Block1 = Levels[1].Block;
		P->W_Block2 = Levels[2].Block;
		P->W_Block3 = Levels[3].Block;
		P->W_TileWake = C.UAV(TWake);
		P->W_TileAwake = C.UAV(TAwake);
		P->W_TileState = C.UAV(TState);
		P->W_TileSlot = C.UAV(TSlot);
		P->W_FrozenCount = C.UAV(FrozenCount);
		P->W_FrozenCap = C.SRV(FrozenCap);
		P->W_Lists = C.UAV(Lists);
		P->W_Counts = C.UAV(Counts);
		P->W_SimIndex = C.UAV(SimIndex);
		P->W_Args = C.ArgsUAV(TArgs);
	};
	{
		auto* P = C.GB.AllocParameters<FWaterTilesParameters>();
		Bind(P);
		P->W_TileTotal = C.SRV(TileTotal);
		P->W_TileBody = C.UAV(Body);
		Run<FWaterTileAwakeCS>(C.GB, TEXT("Water.TileAwake"), P, nullptr, 0, Groups(TileCount));
	}
	{
		auto* P = C.GB.AllocParameters<FWaterTilesParameters>();
		Bind(P);
		Run<FWaterTileStateCS>(C.GB, TEXT("Water.TileState"), P, nullptr, 0, Groups(TileCount));
	}
	{
		auto* P = C.GB.AllocParameters<FWaterTilesParameters>();
		Bind(P);
		Run<FWaterTileArgsCS>(C.GB, TEXT("Water.TileArgs"), P, nullptr, 0, 1);
	}
}

void FSwampWaterSim::TilesRetire(FCtx& C, TRefCountPtr<FRDGPooledBuffer>& TypeBuffer, int32 Block, bool bZeroVelocity)
{
	auto* P = C.GB.AllocParameters<FWaterTilesParameters>();
	SetCommon(C, P->Common, ListRetire);
	P->W_RetireBlock = Block;
	P->W_RetireVelocity = bZeroVelocity ? 1 : 0;
	P->W_RetireType = C.UAV(TypeBuffer);
	Run<FWaterRetireCS>(C.GB, TEXT("Water.Retire"), P, C.Buf(TArgs), ArgRetire);
}

void FSwampWaterSim::TilesUpdateRange(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterTilesParameters>();
	SetCommon(C, P->Common, ListD);
	P->Common.W_TileRange = nullptr; // пишется как TileRangeOut
	P->W_TileTop = C.UAV(Top);
	P->W_TilePrevTop = C.UAV(PrevTop);
	P->W_TileRangeOut = C.UAV(Range);
	P->W_TileBed = C.SRV(TBed);
	Run<FWaterTileRangeCS>(C.GB, TEXT("Water.TileRange"), P, C.Buf(TArgs), ArgDThreads);
}

void FSwampWaterSim::TilesShiftTop(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterTilesParameters>();
	SetCommon(C, P->Common, ListD);
	P->W_TileTop = C.UAV(Top);
	P->W_TilePrevTop = C.UAV(PrevTop);
	Run<FWaterTileTopShiftCS>(C.GB, TEXT("Water.TileTopShift"), P, C.Buf(TArgs), ArgDThreads);
}

// Какие плитки считать, и стенка из тех, что перестали.
void FSwampWaterSim::Schedule(FCtx& C)
{
	TilesSchedule(C);
	TilesRetire(C, Type, TileCells, true);
	for (int32 L = 1; L < Levels.Num(); L++) TilesRetire(C, LType[L], Levels[L].Block, false);
}

// ---------------------------------------------------------------- сетка (WaterGrid)

void FSwampWaterSim::GridGather(FCtx& C)
{
	FRDGTextureRef SurfaceIn = C.Tex(Surface[5]);
	{
		auto* P = C.GB.AllocParameters<FWaterGridParameters>();
		SetCommon(C, P->Common, ListD);
		P->W_BodyFrac = C.UAV(BodyFrac);
		P->W_SurfaceIn = SurfaceIn;
		Run<FWaterBodyCellsCS>(C.GB, TEXT("Water.BodyCells"), P, C.Buf(TArgs), ArgDCells);
	}
	{
		auto* P = C.GB.AllocParameters<FWaterGridParameters>();
		SetCommon(C, P->Common, ListSim);
		P->Common.W_FullIn = nullptr; // пишется как FullOut
		P->W_Mom = C.UAV(Mom);
		P->W_Wgt = C.UAV(Wgt);
		P->Cells.W_Vel = C.UAV(Vel);
		P->W_ValidIn = C.UAV(ValidA);
		P->Cells.W_Density = C.UAV(Density);
		P->W_RestDensity = C.UAV(RestDensity);
		P->W_FullOut = C.UAV(Full);
		P->W_SurfaceIn = SurfaceIn;
		P->W_BodyFracIn = C.SRV(BodyFrac);
		Run<FWaterGatherCS>(C.GB, TEXT("Water.Gather"), P, C.Buf(TArgs), ArgSimCells);
	}
}

void FSwampWaterSim::GridClassify(FCtx& C)
{
	{
		auto* P = C.GB.AllocParameters<FWaterGridParameters>();
		SetCommon(C, P->Common, ListSim);
		P->Cells.W_Density = C.UAV(Density);
		P->Cells.W_Type = C.UAV(Type);
		P->W_P = C.UAV(Pressure);
		Run<FWaterClassifyCS>(C.GB, TEXT("Water.Classify"), P, C.Buf(TArgs), ArgSimCells);
	}
	{
		auto* P = C.GB.AllocParameters<FWaterGridParameters>();
		SetCommon(C, P->Common, ListD);
		P->Cells.W_Type = C.UAV(Type);
		P->Cells.W_Vel = C.UAV(Vel);
		P->W_FaceSolid = C.UAV(FaceSolid);
		P->W_ValidIn = C.UAV(ValidA);
		Run<FWaterFaceBoundaryCS>(C.GB, TEXT("Water.FaceBoundary"), P, C.Buf(TArgs), ArgDCells);
	}
	{
		auto* P = C.GB.AllocParameters<FWaterGridParameters>();
		SetCommon(C, P->Common, ListSim);
		P->Cells.W_Type = C.UAV(Type);
		P->Cells.W_Vel = C.UAV(Vel);
		P->Cells.W_Density = C.UAV(Density);
		P->W_Rhs = C.UAV(Rhs);
		P->W_Diag = C.UAV(Diag);
		P->W_Mask = C.UAV(Mask);
		Run<FWaterDivergenceCS>(C.GB, TEXT("Water.Divergence"), P, C.Buf(TArgs), ArgSimCells);
	}
}

void FSwampWaterSim::GridCorrectionRhs(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterGridParameters>();
	SetCommon(C, P->Common, ListSim);
	P->Cells.W_Type = C.UAV(Type);
	P->Cells.W_Density = C.UAV(Density);
	P->W_Rhs = C.UAV(Rhs);
	P->W_Corr = C.UAV(Correction);
	P->W_RestDensity = C.UAV(RestDensity);
	Run<FWaterCorrectionRhsCS>(C.GB, TEXT("Water.CorrectionRhs"), P, C.Buf(TArgs), ArgSimCells);
}

void FSwampWaterSim::GridRecordRest(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterGridParameters>();
	SetCommon(C, P->Common, ListSim);
	P->W_InitLevel = Config.InitialLevel;
	P->Cells.W_Density = C.UAV(Density);
	P->W_RestDensity = C.UAV(RestDensity);
	Run<FWaterRecordRestCS>(C.GB, TEXT("Water.RecordRest"), P, C.Buf(TArgs), ArgSimCells);
}

void FSwampWaterSim::GridCorrectionFaces(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterGridParameters>();
	SetCommon(C, P->Common, ListD);
	P->Cells.W_Type = C.UAV(Type);
	P->Cells.W_Density = C.UAV(Density);
	P->W_Corr = C.UAV(Correction);
	P->W_Mom = C.UAV(Mom);
	Run<FWaterCorrectionFacesCS>(C.GB, TEXT("Water.CorrectionFaces"), P, C.Buf(TArgs), ArgDCells);
}

void FSwampWaterSim::GridProject(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterGridParameters>();
	SetCommon(C, P->Common, ListSim);
	P->Cells.W_Type = C.UAV(Type);
	P->W_P = C.UAV(Pressure);
	P->Cells.W_Vel = C.UAV(Vel);
	P->W_FaceSolid = C.UAV(FaceSolid);
	P->W_ValidIn = C.UAV(ValidA);
	P->Cells.W_Density = C.UAV(Density);
	Run<FWaterProjectCS>(C.GB, TEXT("Water.Project"), P, C.Buf(TArgs), ArgSimCells);
}

void FSwampWaterSim::GridExtrapolate(FCtx& C)
{
	TRefCountPtr<FRDGPooledBuffer>* From = &ValidA;
	TRefCountPtr<FRDGPooledBuffer>* To = &ValidB;
	for (int32 I = 0; I < ExtrapolationLayers; I++)
	{
		auto* P = C.GB.AllocParameters<FWaterGridParameters>();
		SetCommon(C, P->Common, ListD);
		P->Cells.W_Vel = C.UAV(Vel);
		P->W_ValidIn = C.UAV(*From);
		P->W_ValidOut = C.UAV(*To);
		Run<FWaterExtrapolateCS>(C.GB, TEXT("Water.Extrapolate"), P, C.Buf(TArgs), ArgDCells);
		Swap(From, To);
	}
}

void FSwampWaterSim::GridInitPressure(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterGridParameters>();
	SetCommon(C, P->Common, 0);
	P->W_InitLevel = Config.InitialLevel;
	P->W_P = C.UAV(Pressure);
	Run<FWaterInitPressureCS>(C.GB, TEXT("Water.InitPressure"), P, nullptr, 0, Groups(Cells));
}

// ---------------------------------------------------------------- частицы (WaterParticles)

void FSwampWaterSim::SetCounting(FCtx& C, FWaterParticlesParameters& P)
{
	P.W_Frozen = C.UAV(Frozen);
	P.W_FrozenStart = C.SRV(FrozenStart);
	P.W_FrozenCap = C.SRV(FrozenCap);
	P.W_FrozenCount = C.UAV(FrozenCount);
	P.W_TileSlot = C.SRV(TSlot);
	P.W_TileWake = C.UAV(TWake);
	P.W_TileTop = C.UAV(Top);
	P.W_TileState = C.SRV(TState);
	P.W_Count = C.UAV(KeyCount);
	P.W_Key = C.UAV(Keys);
	P.W_Extra = C.UAV(Extra);
}

void FSwampWaterSim::ParticlesPrepareArgs(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterParticlesParameters>();
	SetCommon(C, P->Common, 0);
	P->W_ParticleCount = C.SRV(PCount);
	P->W_Args = C.ArgsUAV(PArgs);
	Run<FWaterPrepareArgsCS>(C.GB, TEXT("Water.PrepareArgs"), P, nullptr, 0, 1);
}

// Вода до стартового уровня: 2×2×2 частицы на ячейку со случайным сдвигом.
void FSwampWaterSim::ParticlesSeed(FCtx& C)
{
	static uint32 SeedCounter = 0;
	AddClearUAVPass(C.GB, C.UAV(PCount), 0u);
	auto* P = C.GB.AllocParameters<FWaterParticlesParameters>();
	SetCommon(C, P->Common, 0);
	P->W_InitLevel = Config.InitialLevel;
	P->W_Seed = (SeedCounter++) * 7919u;
	P->W_SeedJitter = Config.SeedJitter;
	P->W_Capacity = uint32(Capacity);
	P->W_ParticlesSorted = C.UAV(ParticlesCur);
	P->W_ParticleCountOut = C.UAV(PCount);
	Run<FWaterSeedCS>(C.GB, TEXT("Water.Seed"), P, nullptr, 0, Groups(Cells));
	ParticlesPrepareArgs(C);
}

void FSwampWaterSim::ParticlesClearCounts(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterParticlesParameters>();
	SetCommon(C, P->Common, ListD);
	P->W_Count = C.UAV(KeyCount);
	P->W_Extra = C.UAV(Extra);
	Run<FWaterClearCountsCS>(C.GB, TEXT("Water.ClearCounts"), P, C.Buf(TArgs), ArgDTiles);
}

// Посчитать все частицы заново (после сброса).
void FSwampWaterSim::ParticlesRecount(FCtx& C)
{
	ParticlesClearCounts(C);
	auto* P = C.GB.AllocParameters<FWaterParticlesParameters>();
	SetCommon(C, P->Common, 0);
	SetCounting(C, *P);
	P->W_ParticlesIn = C.SRV(ParticlesCur);
	P->W_ParticleCount = C.SRV(PCount);
	Run<FWaterCountParticlesCS>(C.GB, TEXT("Water.CountParticles"), P, C.Buf(PArgs), 0);
}

// Вернуть частицы проснувшихся плиток, очистить накопители граней, разложить частицы считаемых плиток и каймы по
// ячейкам (вместе с P2G); частицы уснувших — отложить.
void FSwampWaterSim::ParticlesSort(FCtx& C)
{
	FRDGBuilder& GB = C.GB;
	// SortLoad.
	{
		auto* P = GB.AllocParameters<FWaterParticlesParameters>();
		SetCommon(C, P->Common, ListWake);
		P->W_Capacity = uint32(Capacity);
		SetCounting(C, *P);
		P->W_ParticlesRW = C.UAV(ParticlesCur);
		P->W_ParticleCountOut = C.UAV(PCount);
		Run<FWaterLoadFrozenCS>(GB, TEXT("Water.LoadFrozen"), P, C.Buf(TArgs), ArgWake);
	}
	ParticlesPrepareArgs(C);
	TilesUpdateRange(C);
	{
		auto* P = GB.AllocParameters<FWaterParticlesParameters>();
		SetCommon(C, P->Common, ListD);
		P->W_Mom = C.UAV(Mom);
		P->W_Wgt = C.UAV(Wgt);
		Run<FWaterClearFacesCS>(GB, TEXT("Water.ClearFaces"), P, C.Buf(TArgs), ArgDCells);
	}
	// SortScan.
	{
		auto* P = GB.AllocParameters<FWaterParticlesParameters>();
		SetCommon(C, P->Common, ListD);
		P->W_Count = C.UAV(KeyCount);
		P->W_CellStart = C.UAV(CellStart);
		P->W_TileTotal = C.UAV(TileTotal);
		Run<FWaterTileScanCS>(GB, TEXT("Water.TileScan"), P, C.Buf(TArgs), ArgDTiles);
	}
	{
		auto* P = GB.AllocParameters<FWaterParticlesParameters>();
		SetCommon(C, P->Common, ListD);
		P->W_TileTotal = C.UAV(TileTotal);
		P->W_SlotBase = C.UAV(SlotBase);
		P->W_Extra = C.UAV(Extra);
		P->W_ParticleCountOut = C.UAV(NewCount);
		Run<FWaterBaseScanCS>(GB, TEXT("Water.BaseScan"), P, nullptr, 0, 1);
	}
	// SortScatter: раскладка в другой буфер вместе с P2G.
	{
		auto* P = GB.AllocParameters<FWaterParticlesParameters>();
		SetCommon(C, P->Common, ListD);
		P->W_Capacity = uint32(Capacity);
		SetCounting(C, *P);
		P->W_ParticlesIn = C.SRV(ParticlesCur);
		P->W_ParticleCount = C.SRV(PCount);
		P->W_ParticleCountOut = C.UAV(NewCount);
		P->W_CellStartIn = C.SRV(CellStart);
		P->W_SlotBaseIn = C.SRV(SlotBase);
		P->W_ParticlesSorted = C.UAV(ParticlesNext);
		P->W_Mom = C.UAV(Mom);
		P->W_Wgt = C.UAV(Wgt);
		Run<FWaterSortParticlesCS>(GB, TEXT("Water.SortParticles"), P, C.Buf(PArgs), 1);
	}
	Swap(ParticlesCur, ParticlesNext);
	{
		auto* P = GB.AllocParameters<FWaterParticlesParameters>();
		SetCommon(C, P->Common, 0);
		P->W_NewCount = C.SRV(NewCount);
		P->W_ParticleCountOut = C.UAV(PCount);
		P->W_Args = C.ArgsUAV(PArgs);
		Run<FWaterFinishSortCS>(GB, TEXT("Water.FinishSort"), P, nullptr, 0, 1);
	}
	ParticlesClearCounts(C);
	TilesShiftTop(C);
}

// Перенос частиц считаемых плиток на месте (G2P + RK2), заявки белой воды, счёт для следующей сортировки.
void FSwampWaterSim::ParticlesAdvect(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterParticlesParameters>();
	SetCommon(C, P->Common, 0);
	P->W_WhiteRequests = C.UAV(WhiteRequestsBuf);
	P->W_WhiteCapacity = uint32(WhiteRequests);
	P->W_SurfaceIn = C.Tex(Surface[5]);
	P->W_EntrainRate = Config.EntrainRate;
	P->W_EntrainSpeed = Config.EntrainSpeed;
	P->W_BubbleLife = Config.BubbleLifetime;
	P->W_SleepSpeed = Config.SleepSpeed;
	P->W_ParticlesRW = C.UAV(ParticlesCur);
	P->W_ParticleCount = C.SRV(PCount);
	P->W_VelRO = C.SRV(Vel);
	P->W_Stats = C.UAV(Speed);
	P->W_DispIn = C.SRV(Mom);
	P->W_TypeRO = C.SRV(Type);
	P->W_DensityRO = C.SRV(Density);
	SetCounting(C, *P);
	Run<FWaterAdvectCS>(C.GB, TEXT("Water.Advect"), P, C.Buf(PArgs), 0);
}

// ---------------------------------------------------------------- давление (WaterPressure)

void FSwampWaterSim::PressureSolve(FCtx& C, int32 Cycles, TRefCountPtr<FRDGPooledBuffer>* XOverride)
{
	auto LevelType = [&](int32 L) -> TRefCountPtr<FRDGPooledBuffer>& { return L == 0 ? Type : LType[L]; };
	auto LevelX = [&](int32 L) -> TRefCountPtr<FRDGPooledBuffer>& { return L == 0 ? (XOverride ? *XOverride : Pressure) : LX[L]; };
	auto LevelF = [&](int32 L) -> TRefCountPtr<FRDGPooledBuffer>& { return L == 0 ? Rhs : LF[L]; };

	auto Make = [&](int32 Index, int32 Sweeps) -> FWaterPressureParameters*
	{
		auto* P = C.GB.AllocParameters<FWaterPressureParameters>();
		SetCommon(C, P->Common, ListSim);
		P->Cells.W_Density = C.UAV(Density);
		P->W_Diag = C.UAV(Diag);
		P->W_Mask = C.UAV(Mask);
		P->W_SimIndex = C.SRV(SimIndex);
		const FLevel& L = Levels[Index];
		P->W_LShift = L.Shift;
		P->W_LNY = L.NY;
		P->W_LBlock = L.Block;
		P->W_LH = L.H;
		P->W_LFine = Index == 0 ? 1 : 0;
		P->W_Sweeps = Sweeps;
		P->W_LType = C.UAV(LevelType(Index));
		P->W_LX = C.UAV(LevelX(Index));
		P->W_LF = C.UAV(LevelF(Index));
		if (Index + 1 < Levels.Num())
		{
			const FLevel& Cl = Levels[Index + 1];
			P->W_CShift = Cl.Shift;
			P->W_CNY = Cl.NY;
			P->W_CBlock = Cl.Block;
			P->W_CType = C.UAV(LevelType(Index + 1));
			P->W_CX = C.UAV(LevelX(Index + 1));
			P->W_CF = C.UAV(LevelF(Index + 1));
		}
		return P;
	};

	const int32 Last = Levels.Num() - 1;
	FRDGBufferRef Args = C.Buf(TArgs);
	for (int32 Cycle = 0; Cycle < Cycles; Cycle++)
	{
		for (int32 I = 0; I < Last; I++)
		{
			auto* P = Make(I, SmoothSweeps);
			if (I == 0) Run<FWaterMGDownCS>(C.GB, TEXT("Water.MGDown"), P, Args, ArgSimTiles);
			else Run<FWaterMGDownSmallCS>(C.GB, TEXT("Water.MGDownSmall"), P, Args, ArgSimTiles);
		}
		Run<FWaterMGCoarsestCS>(C.GB, TEXT("Water.MGCoarsest"), Make(Last, CoarsestSweeps), nullptr, 0, 1);
		for (int32 I = Last - 1; I >= 0; I--)
		{
			auto* P = Make(I, SmoothSweeps);
			if (I == 0) Run<FWaterMGUpCS>(C.GB, TEXT("Water.MGUp"), P, Args, ArgSimTiles);
			else Run<FWaterMGUpSmallCS>(C.GB, TEXT("Water.MGUpSmall"), P, Args, ArgSimTiles);
		}
	}
}

// ---------------------------------------------------------------- поверхность (WaterHeightField)

void FSwampWaterSim::BuildSurface(FCtx& C, bool bAll)
{
	FRDGBuilder& GB = C.GB;
	FRDGTextureRef Pass[6];
	for (int32 I = 0; I < 6; I++) Pass[I] = C.Tex(Surface[I]);
	FRDGTextureRef Prev = C.Tex(SurfacePrev);
	if (!bAll) AddCopyTexturePass(GB, Pass[5], Prev);

	auto Dispatch = [&](auto Tag, const TCHAR* Name, FWaterHeightFieldParameters* P, int32 Arg)
	{
		using TShader = typename decltype(Tag)::Type;
		if (bAll) Run<TShader>(GB, Name, P, nullptr, 0, NT * NT);
		else Run<TShader>(GB, Name, P, C.Buf(TArgs), Arg);
	};
	auto Make = [&]() -> FWaterHeightFieldParameters*
	{
		auto* P = GB.AllocParameters<FWaterHeightFieldParameters>();
		SetCommon(C, P->Common, ListD);
		P->W_AllTiles = bAll ? 1 : 0;
		P->W_Spread = 0;
		P->W_InitLevel = Config.InitialLevel;
		P->W_SurfaceKeep = Prev;
		P->W_SimIndex = C.SRV(SimIndex);
		return P;
	};
	{
		auto* P = Make();
		P->Cells.W_Density = C.UAV(Density);
		P->Cells.W_Vel = C.UAV(Vel);
		P->W_SurfaceOut = GB.CreateUAV(Pass[0]);
		Dispatch(TTag<FWaterBuildSurfaceCS>(), TEXT("Water.BuildSurface"), P, ArgDTiles);
	}
	for (int32 Blur = 0; Blur < 4; Blur++)
	{
		const bool bAlongX = Blur % 2 == 0;
		auto* P = Make();
		P->W_BlurStep = FIntPoint(bAlongX ? 1 : 0, bAlongX ? 0 : 1);
		P->W_SurfaceIn = Pass[Blur];
		P->W_SurfaceOut = GB.CreateUAV(Pass[Blur + 1]);
		Dispatch(TTag<FWaterBlurSurfaceCS>(), TEXT("Water.BlurSurface"), P, ArgDTiles);
	}
	{
		auto* P = Make();
		P->W_Spread = bAll ? 0 : 1;
		P->W_SurfaceIn = Pass[4];
		P->W_SurfaceOut = GB.CreateUAV(Pass[5]);
		Dispatch(TTag<FWaterFillSurfaceCS>(), TEXT("Water.FillSurface"), P, ArgSpread);
	}
	if (bAll) AddCopyTexturePass(GB, Pass[5], Prev);
}

// ---------------------------------------------------------------- белая вода (WaterWhitewater)

namespace
{
	void BindWhite(FRDGBuilder& GB, FWaterWhitewaterParameters* P, FRDGBufferUAVRef Pool, FRDGBufferUAVRef Free, FRDGBufferUAVRef FreeCount)
	{
		P->W_White = Pool;
		P->W_Free = Free;
		P->W_FreeCount = FreeCount;
	}
}

void FSwampWaterSim::WhiteReset(FCtx& C)
{
	auto* P = C.GB.AllocParameters<FWaterWhitewaterParameters>();
	SetCommon(C, P->Common, 0);
	P->W_PoolSize = uint32(Config.WhitewaterPool);
	BindWhite(C.GB, P, C.UAV(WhitePool), C.UAV(WhiteFree), C.UAV(WhiteFreeCount));
	Run<FWaterResetPoolCS>(C.GB, TEXT("Water.WhiteReset"), P, nullptr, 0, Groups(Config.WhitewaterPool));
	AddCopyBufferPass(C.GB, C.Buf(WhitePrev), C.Buf(WhitePool));
}

void FSwampWaterSim::WhiteUpdate(FCtx& C)
{
	// Прошлое состояние — для отрисовки, которая отстаёт от последнего шага (USwampWaterSubsystem::UpdateWhiteClock).
	AddCopyBufferPass(C.GB, C.Buf(WhitePrev), C.Buf(WhitePool));
	auto* P = C.GB.AllocParameters<FWaterWhitewaterParameters>();
	SetCommon(C, P->Common, 0);
	P->W_PoolSize = uint32(Config.WhitewaterPool);
	P->W_BubbleRise = Config.BubbleRise;
	P->W_VelRO = C.SRV(Vel);
	P->W_SurfaceIn = C.Tex(Surface[5]);
	BindWhite(C.GB, P, C.UAV(WhitePool), C.UAV(WhiteFree), C.UAV(WhiteFreeCount));
	Run<FWaterWhiteUpdateCS>(C.GB, TEXT("Water.WhiteUpdate"), P, nullptr, 0, Groups(Config.WhitewaterPool));
}

// Подшаг: состарить и сдвинуть живые частицы, родить заявленные переносом.
void FSwampWaterSim::WhiteStep(FCtx& C)
{
	WhiteUpdate(C);
	auto* P = C.GB.AllocParameters<FWaterWhitewaterParameters>();
	SetCommon(C, P->Common, 0);
	P->W_WhiteCapacity = uint32(WhiteRequests);
	P->W_WhiteRequests = C.SRV(WhiteRequestsBuf);
	P->W_RequestCount = C.SRV(Speed);
	BindWhite(C.GB, P, C.UAV(WhitePool), C.UAV(WhiteFree), C.UAV(WhiteFreeCount));
	Run<FWaterWhiteEmitCS>(C.GB, TEXT("Water.WhiteEmit"), P, nullptr, 0, Groups(WhiteRequests));
}

// Пузыри от тел, вошедших в воду.
void FSwampWaterSim::WhiteEmitSources(FCtx& C)
{
	const int32 Count = FMath::Min(SourceData.Num() / 2, WhiteSources);
	if (Count == 0) return;
	FRDGBufferRef SourcesBuf = C.GB.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Count * 2), TEXT("Water.Sources"));
	C.GB.QueueBufferUpload(SourcesBuf, SourceData.GetData(), Count * 2 * sizeof(FVector4f));
	auto* P = C.GB.AllocParameters<FWaterWhitewaterParameters>();
	SetCommon(C, P->Common, 0);
	P->W_SourceCount = uint32(Count);
	P->W_BubbleLife = Config.BubbleLifetime;
	P->W_Sources = C.GB.CreateSRV(SourcesBuf);
	BindWhite(C.GB, P, C.UAV(WhitePool), C.UAV(WhiteFree), C.UAV(WhiteFreeCount));
	Run<FWaterEmitSourcesCS>(C.GB, TEXT("Water.EmitSources"), P, nullptr, 0, Groups(Count));
	SourceData.Reset();
}

// ---------------------------------------------------------------- видимость солнца (WaterSun)

FSwampWaterSim::FSunVisibility FSwampWaterSim::BuildSunVisibility(FRDGBuilder& GB, const FVector2f& CenterXZ, const FVector3f& ToSun, const FSunDepth& SunDepth,
	const FSunVisibility* Sediment, float SedimentExtinction)
{
	FSunVisibility Out;
	if (!bResources) return Out;
	FCtx C(GB, *this);
	// Объём: 128 столбцов по стороне (ячейка воды, ~25 м — с запасом на видимое сквозь муть), 32 слоя по 10 см под
	// уровнем воды (глубже воды здесь нет). Угол привязан к ячейкам — тени не плывут при движении глаза.
	const int32 Side = 128, Layers = 32;
	const float Level = Config.InitialLevel;
	Out.CellSize = FVector3f(Cell, 0.1f, Cell);
	Out.Size = FIntVector(Side, Layers, Side);
	Out.Origin = FVector3f(FMath::FloorToFloat(CenterXZ.X / Cell) * Cell - Side * Cell * 0.5f, Level - Layers * 0.1f,
		FMath::FloorToFloat(CenterXZ.Y / Cell) * Cell - Side * Cell * 0.5f);

	// Прямой луч в воде: преломление на ровной воде (n = 1.333).
	const FVector3f L = ToSun.GetSafeNormal();
	const float CosA = FMath::Max(L.Y, 0.f), Eta = 1.f / 1.333f;
	const float SinW2 = Eta * Eta * (1.f - CosA * CosA);
	const FVector3f Horizontal(-L.X, 0.f, -L.Z);
	const FVector3f InWater = Horizontal * Eta + FVector3f(0.f, -FMath::Sqrt(FMath::Max(1.f - SinW2, 0.f)), 0.f);

	FRDGTextureRef Air = GB.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(Side, Side), PF_R16F, FClearValueBinding::White,
		TexCreate_ShaderResource | TexCreate_UAV), TEXT("Water.SunAir"));
	Out.Volume = GB.CreateTexture(FRDGTextureDesc::Create3D(Out.Size, PF_R8, FClearValueBinding::White,
		TexCreate_ShaderResource | TexCreate_UAV), TEXT("Water.SunVisibility"));
	auto Fill = [&](FWaterSunParameters* P)
	{
		SetCommon(C, P->Common, 0);
		P->W_ToSun = L;
		P->W_SunInWater = InWater.GetSafeNormal();
		P->W_SunLevel = Level;
		P->W_VisOrigin = Out.Origin;
		P->W_VisCell = Out.CellSize;
		P->W_VisSize = Out.Size;
		P->W_AirSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->W_SunDepth = SunDepth.Depth ? SunDepth.Depth : GSystemTextures.GetBlackDummy(GB);
		P->W_SunDepthPos = SunDepth.Position;
		P->W_SunDepthForward = SunDepth.Forward;
		P->W_SunDepthRight = SunDepth.Right;
		P->W_SunDepthUp = SunDepth.Up;
		P->W_SunDepthWidth = SunDepth.Depth ? SunDepth.Width : 0.f;
		const bool bSed = Sediment && Sediment->Volume;
		P->W_Sediment = bSed ? Sediment->Volume : GSystemTextures.GetVolumetricBlackDummy(GB);
		P->W_SedimentOrigin = bSed ? Sediment->Origin : FVector3f::ZeroVector;
		P->W_SedimentExtent = bSed ? Sediment->CellSize * FVector3f(Sediment->Size) : FVector3f::ZeroVector;
		P->W_SedimentExtinction = SedimentExtinction;
	};
	{
		auto* P = GB.AllocParameters<FWaterSunParameters>();
		Fill(P);
		P->W_AirOut = GB.CreateUAV(Air);
		P->W_AirIn = GSystemTextures.GetWhiteDummy(GB);
		P->W_VisOut = GB.CreateUAV(Out.Volume);
		FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("Water.SunAir"), Shader<FWaterAirVisibilityCS>(), P, FIntVector(Side / 8, Side / 8, 1));
	}
	{
		auto* P = GB.AllocParameters<FWaterSunParameters>();
		Fill(P);
		P->W_AirOut = GB.CreateUAV(GB.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_R16F, FClearValueBinding::White,
			TexCreate_UAV), TEXT("Water.SunAirUnused")));
		P->W_AirIn = Air;
		P->W_VisOut = GB.CreateUAV(Out.Volume);
		FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("Water.SunVisibility"), Shader<FWaterWaterVisibilityCS>(), P,
			FIntVector(Side / 4, Layers / 4, Side / 4));
	}
	return Out;
}

FSwampWaterSim::FSunVisibility FSwampWaterSim::UpdateSediment(FRDGBuilder& GB, const FVector2f& CenterXZ, float Dt)
{
	FSunVisibility Out;
	if (!bResources) return Out;
	const int32 Side = 128, Layers = 32;
	const float Level = Config.InitialLevel;
	Out.CellSize = FVector3f(Cell, 0.1f, Cell);
	Out.Size = FIntVector(Side, Layers, Side);
	// Объём ездит за глазом шагами по 8 ячеек: реже пересэмплирование (оно размывает облака).
	const float Snap = Cell * 8.f;
	Out.Origin = FVector3f(FMath::FloorToFloat(CenterXZ.X / Snap) * Snap - Side * Cell * 0.5f, Level - Layers * 0.1f,
		FMath::FloorToFloat(CenterXZ.Y / Snap) * Snap - Side * Cell * 0.5f);
	// Второй вид за кадр — тот же ил, без шага.
	if (SedimentFrame == GFrameNumberRenderThread && SedimentThisFrame && SedimentGraph == &GB)
	{
		Out.Origin = SedimentPrevOrigin;
		Out.Volume = SedimentThisFrame;
		return Out;
	}
	SedimentFrame = GFrameNumberRenderThread;

	FCtx C(GB, *this);
	FRDGTextureRef Next = GB.CreateTexture(FRDGTextureDesc::Create3D(Out.Size, PF_R16F, FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV), TEXT("Water.Sediment"));
	auto* P = GB.AllocParameters<FWaterSedimentParameters>();
	SetCommon(C, P->Common, 0);
	P->W_VelRO = C.SRV(Vel);
	const bool bHasPrev = SedimentPrev.IsValid();
	P->W_SedPrev = bHasPrev ? GB.RegisterExternalTexture(SedimentPrev) : GSystemTextures.GetVolumetricBlackDummy(GB);
	P->W_SedSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	P->W_SedOut = GB.CreateUAV(Next);
	P->W_SedOrigin = Out.Origin;
	P->W_SedPrevOrigin = bHasPrev ? SedimentPrevOrigin : Out.Origin;
	P->W_SedCell = Out.CellSize;
	P->W_SedSize = Out.Size;
	P->W_SedDt = FMath::Clamp(Dt, 0.f, 0.1f);
	P->W_SedLevel = Level;
	P->W_SedHasPrev = bHasPrev ? 1 : 0;
	P->W_SedDebug = CVarSwampWaterSedimentDebug.GetValueOnRenderThread();
	// После сброса вода 1–2 с устанавливается из заданного уровня — эти течения размывом не считаются.
	P->W_SedErode = Render.Time > 2.5f ? 1 : 0;
	FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("Water.Sediment"), Shader<FWaterSedimentUpdateCS>(), P, FIntVector(Side / 4, Layers / 4, Side / 4));
	GB.QueueTextureExtraction(Next, &SedimentPrev);
	SedimentPrevOrigin = Out.Origin;
	SedimentThisFrame = Next;
	SedimentGraph = &GB;
	Out.Volume = Next;
	return Out;
}

// ---------------------------------------------------------------- шаг

// Залить всё до стартового уровня, вода в покое (гидростатическое давление).
void FSwampWaterSim::ResetWater(FCtx& C)
{
	RDG_EVENT_SCOPE_STAT(C.GB, WaterReset, "Water.Reset");
	CreateResources(C); // поля, плитки, уровни давления — с нуля
	StepData[0] = FVector4f(0.f, float(ObstacleData.Num()), 1.f, StepData[0].W);
	StepData[1] = FVector4f(0.f, 0.f, 0.f, 0.f);
	C.InvalidateStep();
	ParticlesSeed(C);
	WhiteReset(C);
	GridInitPressure(C);
	Schedule(C);
	ParticlesRecount(C);
	ParticlesSort(C);
	GridGather(C);
	GridRecordRest(C);
	BuildSurface(C, true);
	ParticlesRecount(C); // первый шаг сортирует по этому счёту
}

// Часть подшага (части примерно равны по времени GPU, кадр несёт долю шага): 0 — плитки, сортировка (+ P2G), сбор,
// классификация; 1 — давление; 2 — поправка плотности (на шагах с ней), проекция, продолжение в воздух; 3 — перенос.
void FSwampWaterSim::RunPart(FCtx& C, int32 Part, bool bCorrect)
{
	if (Part == 0)
	{
		RDG_EVENT_SCOPE_STAT(C.GB, WaterSort, "Water.Part0");
		Schedule(C);
		ParticlesSort(C);
		GridGather(C);
		GridClassify(C);
	}
	else if (Part == 1)
	{
		RDG_EVENT_SCOPE_STAT(C.GB, WaterPressure, "Water.Part1");
		PressureSolve(C, Config.PressureCycles, nullptr);
	}
	else if (Part == 2)
	{
		RDG_EVENT_SCOPE_STAT(C.GB, WaterCorrection, "Water.Part2");
		if (bCorrect)
		{
			// Поправка плотности (тот же мультигрид, один V-цикл). Давление уже решено — Rhs свободен.
			GridCorrectionRhs(C);
			PressureSolve(C, CorrectionPressureCycles, &Correction);
			GridCorrectionFaces(C);
		}
		GridProject(C);
		GridExtrapolate(C);
	}
	else
	{
		RDG_EVENT_SCOPE_STAT(C.GB, WaterAdvect, "Water.Part3");
		ParticlesAdvect(C);
	}
}

void FSwampWaterSim::EndStep(FCtx& C, uint32 Frame)
{
	RDG_EVENT_SCOPE_STAT(C.GB, WaterStepEnd, "Water.StepEnd");
	WhiteStep(C);
	QueueWhiteDump(C);
	BuildSurface(C, false);
	// Замеры: скорость, число плиток, поверхность — приходят через кадр-другой.
	if (!bSpeedPending)
	{
		AddEnqueueCopyPass(C.GB, SpeedReadback.Get(), C.Buf(Speed), 8);
		bSpeedPending = true;
	}
	if (!bCountsPending)
	{
		AddEnqueueCopyPass(C.GB, CountsReadback.Get(), C.Buf(Counts), 16);
		bCountsPending = true;
		CountsFrame = Frame;
	}
	if (!bSurfacePending && (++SurfaceReadbackCounter & 1) == 0)
	{
		auto* P = C.GB.AllocParameters<FWaterHeightFieldParameters>();
		SetCommon(C, P->Common, 0);
		P->W_SurfaceIn = C.Tex(Surface[5]);
		P->W_SurfaceBuf = C.UAV(SurfaceBuf);
		FComputeShaderUtils::AddPass(C.GB, RDG_EVENT_NAME("Water.CopySurface"), Shader<FWaterCopySurfaceCS>(), P, FIntVector(N / 8, N / 8, 1));
		AddEnqueueCopyPass(C.GB, SurfaceReadback.Get(), C.Buf(SurfaceBuf), uint32(N * N * sizeof(FVector4f)));
		bSurfacePending = true;
	}
}

void FSwampWaterSim::QueueWhiteDump(FCtx& C)
{
	if (WhiteDumpStep >= Render.WhiteDumpSteps || Render.Time < Render.WhiteDumpStart) return;
	FWhiteDump* Free = nullptr;
	for (FWhiteDump& D : WhiteDumps) if (D.Step < 0) { Free = &D; break; }
	if (!Free && WhiteDumps.Num() < 16)
	{
		Free = &WhiteDumps.AddDefaulted_GetRef();
		Free->Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Water.WhiteDump"));
	}
	if (!Free) { UE_LOG(LogSwampWater, Warning, TEXT("WhiteDump: no free readback, step %d skipped"), WhiteDumpStep++); return; }
	Free->Step = WhiteDumpStep++;
	Free->StepDt = StepData[1].Y;
	Free->Time = FPlatformTime::Seconds();
	AddEnqueueCopyPass(C.GB, Free->Readback.Get(), C.Buf(WhitePool), uint32(Config.WhitewaterPool * 2 * sizeof(FVector4f)));
}

void FSwampWaterSim::PollReadbacks()
{
	if (!bResources) return;
	for (FWhiteDump& D : WhiteDumps)
	{
		if (D.Step < 0 || !D.Readback->IsReady()) continue;
		const uint32 Bytes = uint32(Config.WhitewaterPool * 2 * sizeof(FVector4f));
		const FVector4f* W = static_cast<const FVector4f*>(D.Readback->Lock(Bytes));
		// Живые места: номер места, позиция + жизнь, скорость + радиус.
		TArray<uint8> Out;
		FMemoryWriter Ar(Out);
		int32 Step = D.Step;
		float StepDt = D.StepDt;
		double Time = D.Time;
		Ar << Step << StepDt << Time;
		for (int32 I = 0; W && I < Config.WhitewaterPool; I++)
		{
			if (W[2 * I].W == 0.f) continue;
			FVector4f A = W[2 * I], B = W[2 * I + 1];
			int32 Slot = I;
			Ar << Slot << A << B;
		}
		D.Readback->Unlock();
		FFileHelper::SaveArrayToFile(Out, *FString::Printf(TEXT("%sWhiteDump/s%04d.bin"), *FPaths::ProjectSavedDir(), D.Step));
		D.Step = -1;
	}
	if (bSpeedPending && SpeedReadback->IsReady())
	{
		const uint32* D = static_cast<const uint32*>(SpeedReadback->Lock(8));
		const float S = FMath::IsFinite(*reinterpret_cast<const float*>(&D[0])) ? *reinterpret_cast<const float*>(&D[0]) : 0.f;
		SpeedReadback->Unlock();
		FScopeLock Lock(&Feedback->Lock);
		Feedback->MeasuredSpeed = S;
		bSpeedPending = false;
	}
	if (bCountsPending && CountsReadback->IsReady())
	{
		const uint32* D = static_cast<const uint32*>(CountsReadback->Lock(16));
		const int32 Sim = int32(D[0]), DCount = int32(D[1]);
		CountsReadback->Unlock();
		FScopeLock Lock(&Feedback->Lock);
		Feedback->LastSim = Sim;
		Feedback->LastD = DCount;
		Feedback->LastFrame = CountsFrame;
		Feedback->bLastFrameValid = true;
		bCountsPending = false;
	}
	if (bSurfacePending && SurfaceReadback->IsReady())
	{
		const FVector4f* D = static_cast<const FVector4f*>(SurfaceReadback->Lock(uint32(N * N * sizeof(FVector4f))));
		if (D)
		{
			FScopeLock Lock(&Feedback->Lock);
			Feedback->Surface.SetNumUninitialized(N * N);
			FMemory::Memcpy(Feedback->Surface.GetData(), D, N * N * sizeof(FVector4f));
			Feedback->SurfaceVersion++;
		}
		SurfaceReadback->Unlock();
		bSurfacePending = false;
	}
}

void FSwampWaterSim::Execute_RenderThread(FRHICommandListImmediate& RHICmdList, TArray<FSwampWaterOp>&& Ops)
{
	const double TP = FPlatformTime::Seconds();
	PollReadbacks();
	static double SumPoll = 0, MaxPoll = 0;
	SumPoll += FPlatformTime::Seconds() - TP;
	MaxPoll = FMath::Max(MaxPoll, FPlatformTime::Seconds() - TP);
	if (Ops.Num() == 0 || !IsValid()) return;
	if (!bResources)
	{
		bool bHasReset = false;
		for (const FSwampWaterOp& Op : Ops) bHasReset |= Op.Type == FSwampWaterOp::Reset;
		if (!bHasReset) return;
	}

	const double T0 = FPlatformTime::Seconds();
	FRDGBuilder GB(RHICmdList, RDG_EVENT_NAME("SwampWater"));
	FCtx C(GB, *this);
	for (FSwampWaterOp& Op : Ops)
	{
		switch (Op.Type)
		{
		case FSwampWaterOp::Reset:
			ResetWater(C);
			break;
		case FSwampWaterOp::SetStep:
			StepData[0] = Op.Step0;
			StepData[1] = Op.Step1;
			C.InvalidateStep();
			break;
		case FSwampWaterOp::SetObstacles:
			ObstacleData = MoveTemp(Op.Obstacles);
			C.InvalidateObstacles();
			break;
		case FSwampWaterOp::SetSources:
			SourceData = MoveTemp(Op.Sources);
			break;
		case FSwampWaterOp::FlushSources:
			if (bResources) WhiteEmitSources(C);
			break;
		case FSwampWaterOp::StepStart:
			if (bResources) AddClearUAVPass(GB, C.UAV(Speed), 0u);
			break;
		case FSwampWaterOp::Part:
			if (bResources) RunPart(C, Op.Index, Op.bFlag);
			break;
		case FSwampWaterOp::StepEnd:
			if (bResources) EndStep(C, Op.Frame);
			break;
		case FSwampWaterOp::WhitewaterIdle:
			if (bResources) { WhiteUpdate(C); QueueWhiteDump(C); }
			break;
		}
	}
	const double T1 = FPlatformTime::Seconds();
	GB.Execute();
	const double T2 = FPlatformTime::Seconds();
	static double SumBuild = 0, SumExec = 0, MaxTotal = 0, LastLog = 0;
	static int32 Frames = 0;
	SumBuild += T1 - T0; SumExec += T2 - T1; MaxTotal = FMath::Max(MaxTotal, T2 - T0); Frames++;
	if (T2 - LastLog > 2.0 && CVarSwampWaterRTLog.GetValueOnRenderThread() != 0)
	{
		UE_LOG(LogSwampWater, Log, TEXT("Water RT: poll %.3f ms avg (max %.3f), build %.3f ms, execute %.3f ms avg, max %.3f ms over %d graphs"), SumPoll * 1e3 / Frames, MaxPoll * 1e3, SumBuild * 1e3 / Frames, SumExec * 1e3 / Frames, MaxTotal * 1e3, Frames);
		SumBuild = SumExec = MaxTotal = SumPoll = MaxPoll = 0; Frames = 0; LastLog = T2;
	}
}

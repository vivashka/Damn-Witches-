#include "WaterShaders.h"

#define WATER_IMPLEMENT(ClassName, File, Entry) \
	void ClassName::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment) \
	{ \
		FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment); \
	} \
	IMPLEMENT_GLOBAL_SHADER(ClassName, "/Plugin/SwampWater/Private/" File, Entry, SF_Compute);

WATER_IMPLEMENT(FWaterTileAwakeCS, "WaterTiles.usf", "TileAwake")
WATER_IMPLEMENT(FWaterTileStateCS, "WaterTiles.usf", "TileState")
WATER_IMPLEMENT(FWaterTileArgsCS, "WaterTiles.usf", "TileArgs")
WATER_IMPLEMENT(FWaterRetireCS, "WaterTiles.usf", "Retire")
WATER_IMPLEMENT(FWaterTileRangeCS, "WaterTiles.usf", "TileRange")
WATER_IMPLEMENT(FWaterTileTopShiftCS, "WaterTiles.usf", "TileTopShift")

WATER_IMPLEMENT(FWaterBodyCellsCS, "WaterGrid.usf", "BodyCells")
WATER_IMPLEMENT(FWaterGatherCS, "WaterGrid.usf", "Gather")
WATER_IMPLEMENT(FWaterClassifyCS, "WaterGrid.usf", "Classify")
WATER_IMPLEMENT(FWaterFaceBoundaryCS, "WaterGrid.usf", "FaceBoundary")
WATER_IMPLEMENT(FWaterDivergenceCS, "WaterGrid.usf", "Divergence")
WATER_IMPLEMENT(FWaterProjectCS, "WaterGrid.usf", "Project")
WATER_IMPLEMENT(FWaterCorrectionRhsCS, "WaterGrid.usf", "CorrectionRhs")
WATER_IMPLEMENT(FWaterRecordRestCS, "WaterGrid.usf", "RecordRest")
WATER_IMPLEMENT(FWaterCorrectionFacesCS, "WaterGrid.usf", "CorrectionFaces")
WATER_IMPLEMENT(FWaterExtrapolateCS, "WaterGrid.usf", "Extrapolate")
WATER_IMPLEMENT(FWaterInitPressureCS, "WaterGrid.usf", "InitPressure")

WATER_IMPLEMENT(FWaterClearCountsCS, "WaterParticles.usf", "ClearCounts")
WATER_IMPLEMENT(FWaterClearFacesCS, "WaterParticles.usf", "ClearFaces")
WATER_IMPLEMENT(FWaterLoadFrozenCS, "WaterParticles.usf", "LoadFrozen")
WATER_IMPLEMENT(FWaterCountParticlesCS, "WaterParticles.usf", "CountParticles")
WATER_IMPLEMENT(FWaterTileScanCS, "WaterParticles.usf", "TileScan")
WATER_IMPLEMENT(FWaterBaseScanCS, "WaterParticles.usf", "BaseScan")
WATER_IMPLEMENT(FWaterSortParticlesCS, "WaterParticles.usf", "SortParticles")
WATER_IMPLEMENT(FWaterAdvectCS, "WaterParticles.usf", "Advect")
WATER_IMPLEMENT(FWaterSeedCS, "WaterParticles.usf", "Seed")
WATER_IMPLEMENT(FWaterPrepareArgsCS, "WaterParticles.usf", "PrepareArgs")
WATER_IMPLEMENT(FWaterFinishSortCS, "WaterParticles.usf", "FinishSort")

WATER_IMPLEMENT(FWaterMGDownCS, "WaterPressure.usf", "MGDown")
WATER_IMPLEMENT(FWaterMGUpCS, "WaterPressure.usf", "MGUp")
WATER_IMPLEMENT(FWaterMGCoarsestCS, "WaterPressure.usf", "MGCoarsest")

// Крупные уровни мультигрида — те же ядра с маленькой группой (SMALL_TILE).
#define WATER_IMPLEMENT_SMALL(ClassName, Entry) \
	void ClassName::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment) \
	{ \
		FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment); \
		Environment.SetDefine(TEXT("SMALL_TILE"), 1); \
	} \
	IMPLEMENT_GLOBAL_SHADER(ClassName, "/Plugin/SwampWater/Private/WaterPressure.usf", Entry, SF_Compute);

WATER_IMPLEMENT_SMALL(FWaterMGDownSmallCS, "MGDownSmall")
WATER_IMPLEMENT_SMALL(FWaterMGUpSmallCS, "MGUpSmall")

WATER_IMPLEMENT(FWaterBuildSurfaceCS, "WaterHeightField.usf", "BuildSurface")
WATER_IMPLEMENT(FWaterBlurSurfaceCS, "WaterHeightField.usf", "BlurSurface")
WATER_IMPLEMENT(FWaterFillSurfaceCS, "WaterHeightField.usf", "FillSurface")
WATER_IMPLEMENT(FWaterCopySurfaceCS, "WaterHeightField.usf", "CopySurface")

WATER_IMPLEMENT(FWaterResetPoolCS, "WaterWhitewater.usf", "ResetPool")
WATER_IMPLEMENT(FWaterWhiteUpdateCS, "WaterWhitewater.usf", "Update")
WATER_IMPLEMENT(FWaterWhiteEmitCS, "WaterWhitewater.usf", "Emit")
WATER_IMPLEMENT(FWaterEmitSourcesCS, "WaterWhitewater.usf", "EmitSources")

WATER_IMPLEMENT(FWaterAirVisibilityCS, "WaterSun.usf", "AirVisibility")
WATER_IMPLEMENT(FWaterWaterVisibilityCS, "WaterSun.usf", "WaterVisibility")

WATER_IMPLEMENT(FWaterSedimentUpdateCS, "WaterSediment.usf", "SedimentUpdate")

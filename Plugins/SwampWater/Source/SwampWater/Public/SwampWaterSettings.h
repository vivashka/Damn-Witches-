#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SwampWaterSettings.generated.h"

// Настройки воды (Project Settings → Plugins → Swamp Water). Перенос полей WaterSimulation из Unity: единицы —
// метры и секунды, как в симуляции.
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Swamp Water"))
class SWAMPWATER_API USwampWaterSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USwampWaterSettings() { CategoryName = TEXT("Plugins"); }

	UPROPERTY(Config, EditAnywhere, Category = "Water")
	bool bEnabled = true;

	// Столбцов сетки по стороне ландшафта. Размер ячейки = ширина ландшафта / Resolution.
	UPROPERTY(Config, EditAnywhere, Category = "Water", meta = (ClampMin = 64, ClampMax = 1024))
	int32 Resolution = 512;

	// Стартовый уровень, м: низины ниже него заполняются водой.
	UPROPERTY(Config, EditAnywhere, Category = "Water")
	float InitialLevel = 18.66f;

	// Сколько сетки над стартовым уровнем, м: куда может подняться вода. Выше частицы летят сами.
	UPROPERTY(Config, EditAnywhere, Category = "Water", meta = (ClampMin = 0.1))
	float Headroom = 1.f;

	// Насколько выше стартового уровня вода может набежать на берег, м.
	UPROPERTY(Config, EditAnywhere, Category = "Water", meta = (ClampMin = 0))
	float ShoreReach = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category = "Physics")
	float Gravity = 9.81f;

	// Шагов воды в секунду: вода шагает по своему времени, а не по кадрам.
	UPROPERTY(Config, EditAnywhere, Category = "Physics", meta = (ClampMin = 15, ClampMax = 240))
	float StepRate = 30.f;

	// Подшагов на шаг не больше этого.
	UPROPERTY(Config, EditAnywhere, Category = "Physics", meta = (ClampMin = 1, ClampMax = 8))
	int32 MaxSubsteps = 2;

	// V-циклов мультигрида на шаг давления.
	UPROPERTY(Config, EditAnywhere, Category = "Physics", meta = (ClampMin = 1, ClampMax = 8))
	int32 PressureCycles = 2;

	// Плитка воды засыпает, если самая быстрая частица в ней медленнее, м/с.
	UPROPERTY(Config, EditAnywhere, Category = "Physics", meta = (ClampMin = 0))
	float SleepSpeed = 0.01f;

	// Вода считается только в этом радиусе от игрока, м (0 — вся): дальняя спит и рисуется как есть, просыпается,
	// когда игрок подходит. Ограничивает худший случай — сколько воды может двигаться сразу.
	UPROPERTY(Config, EditAnywhere, Category = "Physics", meta = (ClampMin = 0))
	float SimRadius = 20.f;

	// Разброс частиц при заливке: 0 — ровной решёткой (вода сразу в покое и засыпает), 1 — случайно (как в Unity).
	UPROPERTY(Config, EditAnywhere, Category = "Physics", meta = (ClampMin = 0, ClampMax = 1))
	float SeedJitter = 0.f;

	// Коэффициент шероховатости дна Маннинга n (медленное заросшее болото 0.05–0.08).
	UPROPERTY(Config, EditAnywhere, Category = "Physics", meta = (ClampMin = 0))
	float ManningRoughness = 0.07f;

	// Шаг раскладывается по кадрам частями (3 на подшаг): кадр несёт долю шага, а не весь.
	UPROPERTY(Config, EditAnywhere, Category = "Physics")
	bool bSpreadStepOverFrames = true;

	UPROPERTY(Config, EditAnywhere, Category = "Bodies", meta = (ClampMin = 1, ClampMax = 256))
	int32 MaxObstacles = 64;

	UPROPERTY(Config, EditAnywhere, Category = "Bodies")
	float WaterDensity = 1000.f;

	// Cd в F = ½·ρ·Cd·A·v².
	UPROPERTY(Config, EditAnywhere, Category = "Bodies", meta = (ClampMin = 0))
	float DragCoefficient = 1.f;

	// Коэффициент присоединённой массы Ca (сфера 0.5).
	UPROPERTY(Config, EditAnywhere, Category = "Bodies", meta = (ClampMin = 0))
	float AddedMassCoefficient = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category = "Whitewater")
	bool bDrawWhitewater = true;

	// Скорость, м/с, с которой вода начинает захватывать воздух.
	UPROPERTY(Config, EditAnywhere, Category = "Whitewater", meta = (ClampMin = 0.1))
	float EntrainSpeed = 1.f;

	UPROPERTY(Config, EditAnywhere, Category = "Whitewater", meta = (ClampMin = 0))
	float EntrainRate = 12.f;

	// Средняя жизнь пузыря радиусом 1 см на поверхности, с.
	UPROPERTY(Config, EditAnywhere, Category = "Whitewater", meta = (ClampMin = 0.1))
	float BubbleLifetime = 3.f;

	// Скорость всплытия пузырьков, м/с.
	UPROPERTY(Config, EditAnywhere, Category = "Whitewater", meta = (ClampMin = 0))
	float BubbleRise = 0.15f;

	UPROPERTY(Config, EditAnywhere, Category = "Whitewater", meta = (ClampMin = 1024, ClampMax = 1048576))
	int32 WhitewaterPool = 1 << 17;

	UPROPERTY(Config, EditAnywhere, Category = "Rendering")
	bool bDrawSurface = true;

	// Показывать воду в окне редактора (залита до стартового уровня). На время Play вода редактора выгружается.
	UPROPERTY(Config, EditAnywhere, Category = "Editor")
	bool bShowInEditor = true;

	// Симулировать воду и в окне редактора (иначе она стоит в покое и только рисуется).
	UPROPERTY(Config, EditAnywhere, Category = "Editor")
	bool bSimulateInEditor = false;

	UPROPERTY(Config, EditAnywhere, Category = "Rendering")
	bool bDrawParticles = false;
};

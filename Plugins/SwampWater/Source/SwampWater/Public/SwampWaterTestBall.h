#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SwampWaterTestBall.generated.h"

class USphereComponent;
class UStaticMeshComponent;

// Проверка воды: шар висит над водой, падает (физикой — всплеск, пузыри, выталкивание), лежит и поднимается
// обратно «краном» (сила к точке, идущей вверх) — тело всё время физическое, вода действует и на подъёме. По кругу.
UCLASS()
class SWAMPWATER_API ASwampWaterTestBall : public AActor
{
	GENERATED_BODY()

public:
	ASwampWaterTestBall();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void OnConstruction(const FTransform& Transform) override;

	// Радиус шара, см.
	UPROPERTY(EditAnywhere, Category = "Test Ball", meta = (ClampMin = "5"))
	float Radius = 40.f;

	// Плотность шара, кг/м³ (вода — 1000: больше — тонет, меньше — всплывает и качается).
	UPROPERTY(EditAnywhere, Category = "Test Ball", meta = (ClampMin = "50"))
	float Density = 2000.f;

	// Сколько висеть наверху перед падением, с.
	UPROPERTY(EditAnywhere, Category = "Test Ball", meta = (ClampMin = "0"))
	float HoldTime = 3.f;

	// Сколько пробыть внизу (от отпускания) перед подъёмом, с.
	UPROPERTY(EditAnywhere, Category = "Test Ball", meta = (ClampMin = "0"))
	float DownTime = 5.f;

	// Скорость подъёма, см/с.
	UPROPERTY(EditAnywhere, Category = "Test Ball", meta = (ClampMin = "10"))
	float LiftSpeed = 150.f;

	// Жёсткость «крана» на подъёме: собственная частота пружины, Гц (затухание — критическое).
	UPROPERTY(EditAnywhere, Category = "Test Ball", meta = (ClampMin = "0.1"))
	float LiftFrequency = 1.5f;

private:
	enum class EPhase : uint8 { Hold, Down, Lift };

	void Hold();
	void Release();

	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Sphere;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;

	EPhase Phase = EPhase::Hold;
	float PhaseTime = 0.f;
	FVector Home = FVector::ZeroVector; // точка подвеса — где поставлен актор
	FVector LiftTarget = FVector::ZeroVector;
};

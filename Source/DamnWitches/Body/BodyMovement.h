#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BodyMovement.generated.h"

class UPrimitiveComponent;

// Движение физического персонажа (перенос Mover из Unity). Ноги толкают тело к желаемой скорости с ограниченной
// силой (сцепление с землёй) и мощностью; всё остальное — гравитация, вода, ветер, удары — действует на тело как на
// любое. Поэтому, например, в воде персонаж замедляется сам: тяга ног уравновешивается сопротивлением воды.
// Единицы внутри — метры, секунды, килограммы, ньютоны; с движком — сантиметры.
UCLASS(ClassGroup = (Body), meta = (BlueprintSpawnableComponent))
class UBodyMover : public UActorComponent
{
	GENERATED_BODY()
public:
	UBodyMover();

	UPROPERTY(EditAnywhere, Category = "Movement") float MoveSpeed = 5.f;   // м/с
	UPROPERTY(EditAnywhere, Category = "Movement") float JumpHeight = 1.2f; // м

	// Сцепление с землёй: максимальная сила ног в долях веса. 2 = разгон до 2g.
	UPROPERTY(EditAnywhere, Category = "Legs") float Grip = 2.f;
	// Мощность ног при обычном шаге, Вт. Ограничивает разгон (F ≤ P / v), растёт с множителем скорости.
	UPROPERTY(EditAnywhere, Category = "Legs") float LegPower = 1300.f;
	// За сколько секунд ноги выводят тело на желаемую скорость, если хватает силы.
	UPROPERTY(EditAnywhere, Category = "Legs") float ResponseTime = 0.08f;
	// За сколько секунд ноги «упираются» в постоянную силу (склон, течение, ветер), когда персонаж стоит.
	UPROPERTY(EditAnywhere, Category = "Legs") float HoldTime = 0.25f;
	// Управление в воздухе: доля от силы ног.
	UPROPERTY(EditAnywhere, Category = "Legs", meta = (ClampMin = 0, ClampMax = 1)) float AirControl = 0.15f;

	UPROPERTY(EditAnywhere, Category = "Ground", meta = (ClampMin = 0, ClampMax = 89)) float SlopeLimit = 45.f;
	UPROPERTY(EditAnywhere, Category = "Ground") float GroundCheckDistance = 0.15f; // м

	// Желаемое направление движения на этот кадр (мир, горизонталь; длина до 1).
	void Move(FVector Direction, float Multiplier = 1.f);
	void Jump();
	// Рывок — способность, а не работа ног: пока он идёт, горизонтальная скорость задаётся точно, по окончании
	// гасится до обычной. Вызывать каждый кадр, пока рывок активен (м/с).
	void Dash(const FVector& Velocity);

	bool IsGrounded() const { return bGrounded; }
	float GetGrip() const { return Grip; }
	float GetLegPower() const { return LegPower; }
	float GetMass() const;
	// Куда персонаж хочет идти (ввод), а не куда его несёт.
	FVector GetMoveDirection() const { return MoveDirection; }
	// Скорость тела, м/с.
	FVector GetVelocity() const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	UPrimitiveComponent* Body() const;
	void CheckGround();

	FVector MoveDirection = FVector::ZeroVector;
	float SpeedMultiplier = 1.f;
	bool bJumpRequested = false;
	bool bDashRequested = false;
	bool bWasDashing = false;
	FVector DashVelocity = FVector::ZeroVector;
	FVector GroundNormal = FVector::UpVector;
	FVector HoldForce = FVector::ZeroVector;
	bool bGrounded = false;
};

UCLASS(ClassGroup = (Body), meta = (BlueprintSpawnableComponent))
class UBodyStamina : public UActorComponent
{
	GENERATED_BODY()
public:
	UBodyStamina();

	UPROPERTY(EditAnywhere, Category = "Stamina") float MaxStamina = 100.f;
	UPROPERTY(EditAnywhere, Category = "Stamina") float RegenPerSecond = 15.f;
	UPROPERTY(EditAnywhere, Category = "Stamina") float RegenDelay = 1.f;

	float GetCurrent() const { return Current; }
	bool HasStamina() const { return Current > 0.f; }
	void Drain(float Amount);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	float Current = 100.f;
	float RegenDelayTimer = 0.f;
};

UCLASS(ClassGroup = (Body), meta = (BlueprintSpawnableComponent))
class UBodySprint : public UActorComponent
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Sprint") float SpeedMultiplier = 1.6f;
	UPROPERTY(EditAnywhere, Category = "Sprint") float StaminaCostPerSecond = 15.f;

	bool IsSprinting() const { return bSprinting; }
	void SetSprinting(bool bWantsSprint, float DeltaTime);

private:
	bool bSprinting = false;
};

// Присед — высота тела. Сгибает ноги аниматор, тело (капсула) следует за высотой. Скорость смены высоты — от
// возможностей ног: вниз тело падает не быстрее свободного падения и тормозится ногами не сильнее сцепления, вверх
// ноги поднимают его не быстрее, чем позволяет их мощность (v ≤ P / (m·g)).
UCLASS(ClassGroup = (Body), meta = (BlueprintSpawnableComponent))
class UBodyCrouch : public UActorComponent
{
	GENERATED_BODY()
public:
	UBodyCrouch();

	UPROPERTY(EditAnywhere, Category = "Crouch") float StandHeight = 1.85f; // м
	UPROPERTY(EditAnywhere, Category = "Crouch") float CrouchHeight = 1.2f;
	UPROPERTY(EditAnywhere, Category = "Crouch") float SpeedMultiplier = 0.5f;

	void SetCrouching(bool bCrouching) { bIsCrouching = bCrouching; }
	bool IsCrouching() const { return bIsCrouching; }
	float GetHeight() const { return Height; }
	// Текущая высота тела в долях роста стоя.
	float GetHeightFraction() const { return Height / StandHeight; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool bIsCrouching = false;
	float Height = 1.85f;
	float Velocity = 0.f;
};

UCLASS(ClassGroup = (Body), meta = (BlueprintSpawnableComponent))
class UBodyDasher : public UActorComponent
{
	GENERATED_BODY()
public:
	UBodyDasher();

	// Длина рывка в ширинах тела.
	UPROPERTY(EditAnywhere, Category = "Dash", meta = (ClampMin = 0)) float DashWidths = 2.5f;
	UPROPERTY(EditAnywhere, Category = "Dash") float DashDuration = 0.15f;
	UPROPERTY(EditAnywhere, Category = "Dash") float DashCooldown = 1.f;
	UPROPERTY(EditAnywhere, Category = "Dash") float StaminaCost = 20.f;
	// Ширина тела поперёк, м (плечи).
	UPROPERTY(EditAnywhere, Category = "Dash") float BodyWidth = 0.46f;

	bool IsDashing() const { return DashRemaining > 1e-4f; }
	bool CanDash() const;
	void Dash(FVector Direction);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	FVector DashDirection = FVector::ZeroVector;
	float DashRemaining = 0.f; // сколько ещё пройти, м
	float CooldownTimer = 0.f;
};

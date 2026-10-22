#include "Body/BodyMovement.h"
#include "Components/PrimitiveComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
	float GravityOf(const UWorld* World) { return World ? -World->GetGravityZ() * 0.01f : 9.81f; }
}

// ---------------------------------------------------------------- Mover

UBodyMover::UBodyMover()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

UPrimitiveComponent* UBodyMover::Body() const
{
	return GetOwner() ? Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent()) : nullptr;
}

float UBodyMover::GetMass() const
{
	const UPrimitiveComponent* B = Body();
	return B && B->IsSimulatingPhysics() ? B->GetMass() : 77.f;
}

FVector UBodyMover::GetVelocity() const
{
	const UPrimitiveComponent* B = Body();
	return B ? B->GetPhysicsLinearVelocity() * 0.01 : FVector::ZeroVector;
}

void UBodyMover::Move(FVector Direction, float Multiplier)
{
	Direction.Z = 0.0;
	if (Direction.SizeSquared() > 1.0) Direction.Normalize();
	MoveDirection = Direction;
	SpeedMultiplier = Multiplier;
}

void UBodyMover::Jump()
{
	if (bGrounded) bJumpRequested = true;
}

void UBodyMover::Dash(const FVector& Velocity)
{
	bDashRequested = true;
	DashVelocity = Velocity;
}

void UBodyMover::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UPrimitiveComponent* B = Body();
	if (!B || !B->IsSimulatingPhysics() || DeltaTime <= 0.f) return;

	CheckGround();

	const float G = GravityOf(GetWorld());
	const float Mass = B->GetMass();
	const float Weight = Mass * G;
	const FVector Velocity = GetVelocity();
	const FVector Horizontal(Velocity.X, Velocity.Y, 0.0);
	const FVector Target = MoveDirection * (MoveSpeed * SpeedMultiplier);

	if (bDashRequested)
	{
		B->AddImpulse((DashVelocity - Horizontal) * 100.0, NAME_None, true);
		bWasDashing = true;
		bDashRequested = false;
		bJumpRequested = false;
		return;
	}
	FVector V = Velocity;
	if (bWasDashing)
	{
		// Рывок кончился — гасим скорость до той, с которой персонаж шёл бы сейчас.
		const FVector Change = Horizontal.GetClampedToMaxSize(Target.Size()) - Horizontal;
		B->AddImpulse(Change * 100.0, NAME_None, true);
		V += Change;
		bWasDashing = false;
	}

	const float MaxForce = (bGrounded ? Grip : Grip * AirControl) * Weight;
	const float MaxPower = LegPower * SpeedMultiplier;

	// На земле ноги толкают вдоль поверхности, в воздухе — только по горизонтали.
	const FVector PlaneNormal = bGrounded ? GroundNormal : FVector::UpVector;
	const FVector PlaneVelocity = FVector::VectorPlaneProject(V, PlaneNormal);
	FVector PlaneTarget = FVector::VectorPlaneProject(Target, PlaneNormal);
	if (PlaneTarget.SizeSquared() > 1e-6) PlaneTarget = PlaneTarget.GetSafeNormal() * Target.Size();

	const FVector Error = PlaneTarget - PlaneVelocity;

	// Статическое трение: стоя на месте, ноги упираются ровно с той силой, которая нужна, чтобы не сдвинуться, —
	// откуда бы она ни шла (склон, течение, ветер). Интегральная часть регулятора подстраивается под любую постоянную
	// силу; предел — сцепление с землёй.
	const bool bStanding = bGrounded && PlaneTarget.SizeSquared() < 1e-4;
	if (bStanding)
		HoldForce = FVector::VectorPlaneProject(HoldForce + Mass * Error / (ResponseTime * HoldTime) * DeltaTime, PlaneNormal).GetClampedToMaxSize(MaxForce);
	else
	{
		const float Step = MaxForce * DeltaTime / HoldTime;
		HoldForce = HoldForce.Size() <= Step ? FVector::ZeroVector : HoldForce - HoldForce.GetSafeNormal() * Step;
	}

	FVector Force = (Mass * Error / ResponseTime + HoldForce).GetClampedToMaxSize(MaxForce);

	// Мощность ног ограничивает только разгон: составляющая силы вдоль скорости F ≤ P / v.
	const double Speed = PlaneVelocity.Size();
	if (Speed > 0.5)
	{
		const FVector Along = PlaneVelocity / Speed;
		const double Push = FVector::DotProduct(Force, Along);
		const double MaxPush = MaxPower / Speed;
		if (Push > MaxPush) Force -= Along * (Push - MaxPush);
	}

	B->AddForce(Force * 100.0);

	if (bJumpRequested && bGrounded)
	{
		const float JumpSpeed = FMath::Sqrt(2.f * G * JumpHeight);
		B->AddImpulse(FVector::UpVector * (JumpSpeed - FMath::Max(V.Z, 0.0)) * 100.0, NAME_None, true);
		bGrounded = false;
	}
	bJumpRequested = false;
}

void UBodyMover::CheckGround()
{
	// Тело сдвигается вниз на GroundCheckDistance: если на что-то пологое натыкается — опора есть.
	bGrounded = false;
	GroundNormal = FVector::UpVector;
	const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Body());
	if (!Capsule) return;
	const FVector Start = Capsule->GetComponentLocation();
	const FVector End = Start - FVector(0, 0, GroundCheckDistance * 100.0);
	// Чуть уже тела, чтобы стена сбоку не считалась опорой.
	const float Radius = Capsule->GetScaledCapsuleRadius() * 0.95f;
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Radius, Capsule->GetScaledCapsuleHalfHeight() - Capsule->GetScaledCapsuleRadius() + Radius);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(BodyGround), false, GetOwner());
	TArray<FHitResult> Hits;
	GetWorld()->SweepMultiByChannel(Hits, Start, End, Capsule->GetComponentQuat(), ECC_Pawn, Shape, Params);
	const float MinUp = FMath::Cos(FMath::DegreesToRadians(SlopeLimit));
	float Nearest = TNumericLimits<float>::Max();
	for (const FHitResult& Hit : Hits)
	{
		if (!Hit.bBlockingHit || Hit.ImpactNormal.Z < MinUp || Hit.Distance >= Nearest) continue;
		Nearest = Hit.Distance;
		bGrounded = true;
		GroundNormal = Hit.ImpactNormal;
	}
}

// ---------------------------------------------------------------- Stamina, Sprint

UBodyStamina::UBodyStamina()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UBodyStamina::Drain(float Amount)
{
	Current = FMath::Max(0.f, Current - Amount);
	RegenDelayTimer = RegenDelay;
}

void UBodyStamina::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (RegenDelayTimer > 0.f) { RegenDelayTimer -= DeltaTime; return; }
	Current = FMath::Min(MaxStamina, Current + RegenPerSecond * DeltaTime);
}

void UBodySprint::SetSprinting(bool bWantsSprint, float DeltaTime)
{
	UBodyStamina* Stamina = GetOwner()->FindComponentByClass<UBodyStamina>();
	bSprinting = bWantsSprint && Stamina && Stamina->HasStamina();
	if (bSprinting) Stamina->Drain(StaminaCostPerSecond * DeltaTime);
}

// ---------------------------------------------------------------- Crouch

UBodyCrouch::UBodyCrouch()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UBodyCrouch::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const UBodyMover* Mover = GetOwner()->FindComponentByClass<UBodyMover>();
	if (!Mover) return;
	const float G = GravityOf(GetWorld());
	const float Target = bIsCrouching ? CrouchHeight : StandHeight;
	const float Remaining = Target - Height;
	if (FMath::Abs(Remaining) < 1e-5f) { Velocity = 0.f; return; }

	if (Remaining < 0.f)
	{
		// Вниз: ноги отпускают — разгон до g; тормозить ноги могут с ускорением до grip·g, поэтому скорость не больше
		// той, с которой они ещё успеют остановить тело на месте.
		const float MaxSpeed = FMath::Sqrt(2.f * Mover->GetGrip() * G * -Remaining);
		Velocity = FMath::Max(Velocity - G * DeltaTime, -MaxSpeed);
	}
	else
	{
		// Вверх: ноги толкают с силой до grip·m·g (сверх веса — (grip − 1)·g) и мощностью P; у цели скорость гасит
		// уже гравитация.
		const float MaxSpeed = FMath::Min(Mover->GetLegPower() / (Mover->GetMass() * G), FMath::Sqrt(2.f * G * Remaining));
		Velocity = FMath::Min(Velocity + (Mover->GetGrip() - 1.f) * G * DeltaTime, MaxSpeed);
	}

	Height += Velocity * DeltaTime;
	if ((Remaining < 0.f && Height < Target) || (Remaining > 0.f && Height > Target))
	{
		Height = Target;
		Velocity = 0.f;
	}
}

// ---------------------------------------------------------------- Dash

UBodyDasher::UBodyDasher()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

bool UBodyDasher::CanDash() const
{
	const UBodyStamina* Stamina = GetOwner()->FindComponentByClass<UBodyStamina>();
	return CooldownTimer <= 0.f && Stamina && Stamina->GetCurrent() >= StaminaCost;
}

void UBodyDasher::Dash(FVector Direction)
{
	if (!CanDash()) return;
	Direction.Z = 0.0;
	if (Direction.SizeSquared() < 1e-4) return;
	GetOwner()->FindComponentByClass<UBodyStamina>()->Drain(StaminaCost);
	DashDirection = Direction.GetSafeNormal();
	DashRemaining = DashWidths * BodyWidth;
	CooldownTimer = DashCooldown;
}

// Рывок ведётся по шагам: на каждый шаг — один запрос скорости ногам (Mover.Dash) и ровно столько же списывается
// с пути (тик до Mover — см. APhysicsCharacter::BeginPlay).
void UBodyDasher::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (CooldownTimer > 0.f) CooldownTimer -= DeltaTime;
	if (!IsDashing() || DeltaTime <= 0.f) return;
	UBodyMover* Mover = GetOwner()->FindComponentByClass<UBodyMover>();
	if (!Mover) return;
	const float Speed = DashWidths * BodyWidth / FMath::Max(DashDuration, 1e-4f);
	const FVector Step = DashDirection * FMath::Min(Speed, DashRemaining / DeltaTime);
	Mover->Dash(Step);
	DashRemaining = FMath::Max(0.f, DashRemaining - float(Step.Size()) * DeltaTime);
}

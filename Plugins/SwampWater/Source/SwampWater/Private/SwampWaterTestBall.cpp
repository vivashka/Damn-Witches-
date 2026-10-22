#include "SwampWaterTestBall.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"

ASwampWaterTestBall::ASwampWaterTestBall()
{
	PrimaryActorTick.bCanEverTick = true;

	// Воде нужна сфера-коллизия (вытеснение и силы считаются по ней), меш — только картинка.
	Sphere = CreateDefaultSubobject<USphereComponent>(TEXT("Sphere"));
	Sphere->SetCollisionProfileName(TEXT("PhysicsActor"));
	Sphere->SetMobility(EComponentMobility::Movable);
	RootComponent = Sphere;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Sphere);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded()) Mesh->SetStaticMesh(SphereMesh.Object);
}

void ASwampWaterTestBall::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Sphere->SetSphereRadius(Radius);
	Mesh->SetRelativeScale3D(FVector(Radius / 50.f)); // базовая сфера — 100 см в поперечнике
}

void ASwampWaterTestBall::BeginPlay()
{
	Super::BeginPlay();
	Home = GetActorLocation();
	const float VolumeM3 = 4.f / 3.f * PI * FMath::Cube(Radius * 0.01f);
	Sphere->SetMassOverrideInKg(NAME_None, Density * VolumeM3, true);
	Hold();
}

void ASwampWaterTestBall::Hold()
{
	Sphere->SetSimulatePhysics(false);
	SetActorLocationAndRotation(Home, FQuat::Identity, false, nullptr, ETeleportType::ResetPhysics);
	Phase = EPhase::Hold;
	PhaseTime = 0.f;
}

void ASwampWaterTestBall::Release()
{
	Sphere->SetSimulatePhysics(true);
	Sphere->SetPhysicsLinearVelocity(FVector::ZeroVector);
	Sphere->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
	Phase = EPhase::Down;
	PhaseTime = 0.f;
}

void ASwampWaterTestBall::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	PhaseTime += DeltaTime;

	switch (Phase)
	{
	case EPhase::Hold:
		if (PhaseTime >= HoldTime) Release();
		break;

	case EPhase::Down:
		if (PhaseTime >= DownTime)
		{
			Phase = EPhase::Lift;
			PhaseTime = 0.f;
			LiftTarget = FVector(Home.X, Home.Y, GetActorLocation().Z);
		}
		break;

	case EPhase::Lift:
	{
		// Точка подвеса едет вверх; шар тянет к ней критически задемпфированная пружина (ускорение, не сила, —
		// не зависит от массы) плюс компенсация тяжести. Вода (Архимед, сопротивление, присоединённая масса) — сверху.
		const float Rise = FMath::Min(LiftSpeed * DeltaTime, float(Home.Z - LiftTarget.Z));
		LiftTarget.Z += Rise;
		const FVector TargetVelocity(0.f, 0.f, Rise < LiftSpeed * DeltaTime ? 0.f : LiftSpeed);
		const float W = 2.f * PI * LiftFrequency;
		const FVector P = Sphere->GetComponentLocation(), V = Sphere->GetPhysicsLinearVelocity();
		const FVector Accel = W * W * (LiftTarget - P) + 2.f * W * (TargetVelocity - V) - FVector(0.f, 0.f, GetWorld()->GetGravityZ());
		Sphere->AddForce(Accel, NAME_None, true);
		Sphere->SetPhysicsAngularVelocityInDegrees(Sphere->GetPhysicsAngularVelocityInDegrees() * FMath::Exp(-3.f * DeltaTime));
		if ((LiftTarget.Z >= Home.Z && FVector::Dist(P, Home) < 5.f && V.Size() < 20.f) || PhaseTime > 30.f) Hold();
		break;
	}
	}
}

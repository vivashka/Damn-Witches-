#include "Body/PhysicsCharacter.h"
#include "Body/BodyMovement.h"
#include "Body/BodyAnimator.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/ConstructorHelpers.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarBodyThirdPerson(TEXT("body.ThirdPerson"), 0,
	TEXT("Camera behind the physics character (debug): see the body, its gait and the water around it."));
static TAutoConsoleVariable<int32> CVarBodyForceCrouch(TEXT("body.ForceCrouch"), 0, TEXT("Hold the physics character crouched (debug)."));

APhysicsCharacter::APhysicsCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	bUseControllerRotationYaw = false;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;

	// Тело для столкновений с миром. Воде оно не видно (SwampWaterIgnore): её видят сегменты тела.
	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Body"));
	Capsule->InitCapsuleSize(BodyRadius * 100.f, 92.5f);
	Capsule->SetCollisionProfileName(TEXT("Pawn"));
	Capsule->BodyInstance.bLockXRotation = true;
	Capsule->BodyInstance.bLockYRotation = true;
	Capsule->BodyInstance.bLockZRotation = true;
	Capsule->BodyInstance.DOFMode = EDOFMode::SixDOF;
	Capsule->BodyInstance.bUseCCD = true;
	Capsule->ComponentTags.Add(TEXT("SwampWaterIgnore"));
	RootComponent = Capsule;

	BodyMesh = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(Capsule);
	BodyMesh->SetRelativeLocation(FVector(0, 0, -92.5f));
	BodyMesh->SetUsingAbsoluteRotation(true);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MannyMesh(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"));
	if (MannyMesh.Succeeded()) BodyMesh->SetSkinnedAssetAndUpdate(MannyMesh.Object);
	// Полное тело видят другие, отражения и тени; своя камера — копию без головы (камера внутри головы).
	BodyMesh->SetOwnerNoSee(true);

	ViewMesh = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("ViewMesh"));
	ViewMesh->SetupAttachment(BodyMesh);
	ViewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ViewMesh->SetOnlyOwnerSee(true);
	ViewMesh->SetCastShadow(false);
	if (MannyMesh.Succeeded()) ViewMesh->SetSkinnedAssetAndUpdate(MannyMesh.Object);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Capsule);
	Camera->SetRelativeLocation(FVector(0, 0, (EyeHeight - 0.925f) * 100.f));
	Camera->bUsePawnControlRotation = true;

	Mover = CreateDefaultSubobject<UBodyMover>(TEXT("Mover"));
	Animator = CreateDefaultSubobject<UBodyAnimator>(TEXT("Animator"));
	Stamina = CreateDefaultSubobject<UBodyStamina>(TEXT("Stamina"));
	Sprint = CreateDefaultSubobject<UBodySprint>(TEXT("Sprint"));
	Crouch = CreateDefaultSubobject<UBodyCrouch>(TEXT("Crouch"));
	Dasher = CreateDefaultSubobject<UBodyDasher>(TEXT("Dasher"));

	static ConstructorHelpers::FObjectFinder<UInputAction> Move(TEXT("/Game/Input/Actions/IA_Move"));
	static ConstructorHelpers::FObjectFinder<UInputAction> Look(TEXT("/Game/Input/Actions/IA_Look"));
	static ConstructorHelpers::FObjectFinder<UInputAction> MouseLook(TEXT("/Game/Input/Actions/IA_MouseLook"));
	static ConstructorHelpers::FObjectFinder<UInputAction> JumpIA(TEXT("/Game/Input/Actions/IA_Jump"));
	MoveAction = Move.Object;
	LookAction = Look.Object;
	MouseLookAction = MouseLook.Object;
	JumpAction = JumpIA.Object;
}

void APhysicsCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Без трения о поверхность: сцеплением с землёй управляют ноги (Grip), а не материал.
	UPhysicalMaterial* Material = NewObject<UPhysicalMaterial>(this);
	Material->Friction = 0.f;
	Material->StaticFriction = 0.f;
	Material->FrictionCombineMode = EFrictionCombineMode::Min;
	Material->bOverrideFrictionCombineMode = true;
	Material->Restitution = 0.f;
	Material->RestitutionCombineMode = EFrictionCombineMode::Min;
	Material->bOverrideRestitutionCombineMode = true;
	Capsule->SetPhysMaterialOverride(Material);
	Capsule->SetMassOverrideInKg(NAME_None, Mass, true);
	Capsule->SetLinearDamping(0.f);
	Capsule->SetSimulatePhysics(true);

	// Порядок за кадр: ввод (пешка) → рывок → ноги → физика → анимация → поза меша.
	Mover->AddTickPrerequisiteActor(this);
	Mover->AddTickPrerequisiteComponent(Dasher);
	Dasher->AddTickPrerequisiteActor(this);
	BodyMesh->SetTickGroup(TG_PostPhysics);
	BodyMesh->AddTickPrerequisiteComponent(Animator);
	ViewMesh->SetTickGroup(TG_PostPhysics);
	ViewMesh->AddTickPrerequisiteComponent(Animator);

	BodyMesh->SetWorldRotation(FRotator(0.f, GetActorRotation().Yaw - 90.f, 0.f));
	Animator->Init(BodyMesh, ViewMesh);
	Animator->SetEyeFollower(Camera, EyeHeight);
	BuildSegments();
	AppliedHalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
}

// Сегменты тела (размеры — Unity PlayerBodyBuilder): капсулы конечностей между суставами, коробки таза, груди, кистей
// и стоп, сфера головы. Привязаны к костям — двигаются с анимацией. Только для запросов (физика их не трогает,
// тело для мира — капсула), вода видит их как тела (тип PhysicsBody) и прикладывает силы к телу-капсуле.
void APhysicsCharacter::BuildSegments()
{
	const FTransform MeshT = BodyMesh->GetComponentTransform();
	auto At = [this](FName Bone) { return BodyMesh->GetBoneTransformByName(Bone, EBoneSpaces::WorldSpace).GetLocation(); };
	const FVector Up = MeshT.GetUnitAxis(EAxis::Z), Forward = MeshT.GetUnitAxis(EAxis::Y);

	auto Setup = [this](UShapeComponent* S, FName Bone)
	{
		S->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		S->SetCollisionObjectType(ECC_PhysicsBody);
		S->SetCollisionResponseToAllChannels(ECR_Ignore);
		S->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
		S->SetGenerateOverlapEvents(false);
		S->SetCanEverAffectNavigation(false);
		S->RegisterComponent();
		S->AttachToComponent(BodyMesh, FAttachmentTransformRules::KeepWorldTransform, Bone);
	};
	auto Limb = [&](FName Bone, FName Child, float Radius)
	{
		const FVector A = At(Bone), B = At(Child);
		UCapsuleComponent* C = NewObject<UCapsuleComponent>(this);
		C->InitCapsuleSize(Radius * 100.f, FMath::Max(float((B - A).Size()) * 0.5f, Radius * 100.f));
		C->SetWorldLocationAndRotation((A + B) * 0.5, FRotationMatrix::MakeFromZ(B - A).ToQuat());
		Setup(C, Bone);
	};
	auto Box = [&](FName Bone, const FVector& Center, const FVector& SizeM)
	{
		UBoxComponent* C = NewObject<UBoxComponent>(this);
		C->InitBoxExtent(FVector(SizeM.X, SizeM.Y, SizeM.Z) * 50.0); // в осях меша: X поперёк, Y вперёд, Z вверх
		C->SetWorldLocationAndRotation(Center, MeshT.GetRotation());
		Setup(C, Bone);
	};

	Box("pelvis", At("pelvis"), FVector(0.32, 0.19, 0.20));
	Box("spine_03", (At("spine_01") + At("neck_01")) * 0.5, FVector(0.36, 0.19, 0.42));
	Limb("neck_01", "head", 0.055f);
	{
		USphereComponent* Head = NewObject<USphereComponent>(this);
		Head->InitSphereRadius(10.5f);
		Head->SetWorldLocation(At("head") + Up * 9.0 + Forward * 2.0);
		Setup(Head, "head");
	}
	for (const TCHAR* S : { TEXT("_l"), TEXT("_r") })
	{
		auto N = [S](const TCHAR* Base) { return FName(FString(Base) + S); };
		Limb(N(TEXT("upperarm")), N(TEXT("lowerarm")), 0.046f);
		Limb(N(TEXT("lowerarm")), N(TEXT("hand")), 0.039f);
		{
			const FVector Hand = At(N(TEXT("hand"))), Dir = (Hand - At(N(TEXT("lowerarm")))).GetSafeNormal();
			UBoxComponent* C = NewObject<UBoxComponent>(this);
			C->InitBoxExtent(FVector(1.75, 4.0, 8.5));
			C->SetWorldLocationAndRotation(Hand + Dir * 8.5, FRotationMatrix::MakeFromZ(Dir).ToQuat());
			Setup(C, N(TEXT("hand")));
		}
		Limb(N(TEXT("thigh")), N(TEXT("calf")), 0.075f);
		Limb(N(TEXT("calf")), N(TEXT("foot")), 0.051f);
		Box(N(TEXT("foot")), At(N(TEXT("foot"))) + Forward * 7.0 - Up * 3.5, FVector(0.085, 0.22, 0.065));
	}
}

FVector APhysicsCharacter::ForwardOnGround() const
{
	const float Yaw = float(GetControlRotation().Yaw);
	return FVector(FMath::Cos(FMath::DegreesToRadians(Yaw)), FMath::Sin(FMath::DegreesToRadians(Yaw)), 0.0);
}

void APhysicsCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	FVector Direction = ConsumeMovementInputVector();
	Direction.Z = 0.0;
	if (Direction.SizeSquared() > 1.0) Direction.Normalize();
	LastDirection = Direction;

	Crouch->SetCrouching(bWantsCrouch || CVarBodyForceCrouch.GetValueOnGameThread() != 0);
	if (Dasher->IsDashing())
	{
		// Рывок ведёт сам Dasher.
	}
	else if (Crouch->IsCrouching())
		Mover->Move(Direction, Crouch->SpeedMultiplier);
	else
	{
		Sprint->SetSprinting(bWantsSprint && Direction.SizeSquared() > 1e-4, DeltaTime);
		Mover->Move(Direction, Sprint->IsSprinting() ? Sprint->SpeedMultiplier : 1.f);
	}
	UpdateHeight();
	UpdateView();
}

// Вид от первого лица — из головы; отладочный от третьего — с 3.5 м позади по взгляду.
void APhysicsCharacter::UpdateView()
{
	const bool bThird = CVarBodyThirdPerson.GetValueOnGameThread() != 0;
	if (bThird != bThirdPerson)
	{
		bThirdPerson = bThird;
		BodyMesh->SetOwnerNoSee(!bThird);
		ViewMesh->SetVisibility(!bThird);
		Animator->SetEyeFollower(bThird ? nullptr : Camera.Get(), EyeHeight);
	}
	if (bThird)
		Camera->SetWorldLocation(GetActorLocation() + FVector(0, 0, 60) - GetControlRotation().Vector() * 350.0);
}

// Высота тела (присед): капсула держит низ на месте, камера и меш следуют.
void APhysicsCharacter::UpdateHeight()
{
	const float Height = Crouch->GetHeight();
	const float Half = Height * 50.f;
	if (FMath::Abs(Half - AppliedHalfHeight) < 0.2f) return;
	const float Delta = AppliedHalfHeight - Half;
	Capsule->SetCapsuleHalfHeight(FMath::Max(Half, Capsule->GetUnscaledCapsuleRadius()));
	AddActorWorldOffset(FVector(0, 0, -Delta), false, nullptr, ETeleportType::TeleportPhysics);
	BodyMesh->SetRelativeLocation(FVector(0, 0, -Half));
	AppliedHalfHeight = Half;
}

void APhysicsCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input) return;

	// Спринт, присед и рывок — свои действия и раскладка (в шаблоне их нет).
	auto Action = [this](const TCHAR* Name)
	{
		UInputAction* A = NewObject<UInputAction>(this, Name);
		A->ValueType = EInputActionValueType::Boolean;
		return A;
	};
	SprintAction = Action(TEXT("IA_BodySprint"));
	CrouchAction = Action(TEXT("IA_BodyCrouch"));
	DashAction = Action(TEXT("IA_BodyDash"));
	BodyMapping = NewObject<UInputMappingContext>(this, TEXT("IMC_Body"));
	BodyMapping->MapKey(SprintAction, EKeys::LeftShift);
	BodyMapping->MapKey(CrouchAction, EKeys::C);
	BodyMapping->MapKey(DashAction, EKeys::LeftControl);
	if (const APlayerController* PC = Cast<APlayerController>(GetController()))
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
			Subsystem->AddMappingContext(BodyMapping, 1);

	Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &APhysicsCharacter::MoveInput);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &APhysicsCharacter::LookInput);
	Input->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &APhysicsCharacter::LookInput);
	Input->BindAction(JumpAction, ETriggerEvent::Started, this, &APhysicsCharacter::JumpPressed);
	Input->BindAction(SprintAction, ETriggerEvent::Started, this, &APhysicsCharacter::SprintPressed);
	Input->BindAction(SprintAction, ETriggerEvent::Completed, this, &APhysicsCharacter::SprintReleased);
	Input->BindAction(CrouchAction, ETriggerEvent::Started, this, &APhysicsCharacter::CrouchPressed);
	Input->BindAction(CrouchAction, ETriggerEvent::Completed, this, &APhysicsCharacter::CrouchReleased);
	Input->BindAction(DashAction, ETriggerEvent::Started, this, &APhysicsCharacter::DashPressed);
}

void APhysicsCharacter::MoveInput(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	const FVector Forward = ForwardOnGround();
	const FVector Right(-Forward.Y, Forward.X, 0.0);
	AddMovementInput(Forward, float(Axis.Y));
	AddMovementInput(Right, float(Axis.X));
}

void APhysicsCharacter::LookInput(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(float(Axis.X));
	AddControllerPitchInput(float(Axis.Y));
}

void APhysicsCharacter::JumpPressed()
{
	Mover->Jump();
}

void APhysicsCharacter::DashPressed()
{
	Dasher->Dash(LastDirection.SizeSquared() > 1e-4 ? LastDirection : ForwardOnGround());
}

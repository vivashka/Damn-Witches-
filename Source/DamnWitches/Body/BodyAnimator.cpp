#include "Body/BodyAnimator.h"
#include "Body/BodyMovement.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarBodyDebug(TEXT("body.Debug"), 0, TEXT("Log the physics character state every frame."));

UBodyAnimator::UBodyAnimator()
{
	PrimaryComponentTick.bCanEverTick = true;
	// После физики: скорость и место тела этого кадра.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

UBodyAnimator::FBone UBodyAnimator::Bone(FName Name) const
{
	FBone B;
	B.Name = Name;
	if (const UPoseableMeshComponent* M = Mesh.Get(); M && M->GetSkinnedAsset())
	{
		B.Index = M->GetSkinnedAsset()->GetRefSkeleton().FindBoneIndex(Name);
		if (B.Index != INDEX_NONE) B.Ref = RefComponent[B.Index];
	}
	return B;
}

void UBodyAnimator::Init(UPoseableMeshComponent* InMesh, UPoseableMeshComponent* InViewMesh)
{
	Mesh = InMesh;
	ViewMesh = InViewMesh;
	if (InViewMesh) InViewMesh->SetBoneScaleByName("head", FVector(0.01), EBoneSpaces::ComponentSpace);
	bReady = false;
	if (!InMesh || !InMesh->GetSkinnedAsset()) return;
	const FReferenceSkeleton& Skeleton = InMesh->GetSkinnedAsset()->GetRefSkeleton();
	const TArray<FTransform>& Local = Skeleton.GetRefBonePose();
	RefComponent.SetNum(Local.Num());
	for (int32 I = 0; I < Local.Num(); I++)
	{
		const int32 Parent = Skeleton.GetParentIndex(I);
		RefComponent[I] = Parent == INDEX_NONE ? Local[I] : Local[I] * RefComponent[Parent];
	}

	Pelvis = Bone("pelvis");
	Chest = Bone("spine_03");
	Head = Bone("head");
	ThighL = Bone("thigh_l"); CalfL = Bone("calf_l"); FootL = Bone("foot_l");
	ThighR = Bone("thigh_r"); CalfR = Bone("calf_r"); FootR = Bone("foot_r");
	UpperL = Bone("upperarm_l"); ForeL = Bone("lowerarm_l"); HandL = Bone("hand_l");
	UpperR = Bone("upperarm_r"); ForeR = Bone("lowerarm_r"); HandR = Bone("hand_r");
	for (const FBone* B : { &Pelvis, &Chest, &Head, &ThighL, &CalfL, &FootL, &ThighR, &CalfR, &FootR, &UpperL, &ForeL, &HandL, &UpperR, &ForeR, &HandR })
		if (B->Index == INDEX_NONE)
		{
			UE_LOG(LogTemp, Warning, TEXT("BodyAnimator: no bone %s"), *B->Name.ToString());
			return;
		}

	// Манекен смотрит вдоль +Y меша, вверх — +Z. Поперечная ось — та, поворот вокруг которой на +угол уводит
	// опущенную конечность вперёд: Side = (−Z) × Forward.
	Side = FVector::CrossProduct(FVector(0, 0, -1), FVector(0, 1, 0));

	// Звенья ноги — по скелету, м.
	ThighLength = float((CalfL.Ref.GetLocation() - ThighL.Ref.GetLocation()).Size() * 0.01);
	ShinLength = float((FootL.Ref.GetLocation() - CalfL.Ref.GetLocation()).Size() * 0.01);
	AnkleHeight = float(FootL.Ref.GetLocation().Z * 0.01);
	HipHeight = AnkleHeight + ThighLength + ShinLength;

	// Руки манекена разведены (A-поза); в покое они висят вдоль тела, чуть в стороны (6°).
	auto Rest = [this](const FBone& From, const FBone& To) -> FQuat
	{
		const FVector Dir = (To.Ref.GetLocation() - From.Ref.GetLocation()).GetSafeNormal();
		const double Out = FMath::Sign(Dir.X);
		const FVector Hang = (FVector(0, 0, -1) * FMath::Cos(FMath::DegreesToRadians(6.0)) + FVector(Out, 0, 0) * FMath::Sin(FMath::DegreesToRadians(6.0))).GetSafeNormal();
		return FQuat::FindBetweenNormals(Dir, Hang);
	};
	UpperRestL = Rest(UpperL, ForeL); ForeRestL = Rest(ForeL, HandL);
	UpperRestR = Rest(UpperR, ForeR); ForeRestR = Rest(ForeR, HandR);

	if (const AActor* Owner = GetOwner()) BodyYaw = float(Owner->GetActorRotation().Yaw);
	bReady = true;
}

void UBodyAnimator::SetRotation(const FBone& B, const FQuat& Q)
{
	UPoseableMeshComponent* M = Mesh.Get();
	FTransform T = M->GetBoneTransformByName(B.Name, EBoneSpaces::ComponentSpace);
	T.SetRotation(Q);
	Apply(B.Name, T);
}

void UBodyAnimator::Apply(FName BoneName, const FTransform& T)
{
	Mesh->SetBoneTransformByName(BoneName, T, EBoneSpaces::ComponentSpace);
	if (UPoseableMeshComponent* V = ViewMesh.Get()) V->SetBoneTransformByName(BoneName, T, EBoneSpaces::ComponentSpace);
}

void UBodyAnimator::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UPoseableMeshComponent* M = Mesh.Get();
	if (!bReady || !M || DeltaTime <= 0.f) return;
	const UBodyMover* Mover = GetOwner()->FindComponentByClass<UBodyMover>();
	const UBodyCrouch* Crouch = GetOwner()->FindComponentByClass<UBodyCrouch>();
	Time += DeltaTime;

	const FVector Velocity = Mover ? Mover->GetVelocity() : FVector::ZeroVector;
	const FVector Horizontal(Velocity.X, Velocity.Y, 0.0);
	const float Speed = float(Horizontal.Size());
	const bool bGrounded = !Mover || Mover->IsGrounded();

	// Тело смотрит туда, куда персонаж хочет идти. Не по фактической скорости: иначе снос водой поворачивал бы тело,
	// руки и ноги гребли бы воду, и тело сносило бы дальше — раскачка.
	const FVector Intent = Mover ? Mover->GetMoveDirection() : Horizontal;
	if (Intent.SizeSquared() > 0.01)
	{
		const float Target = float(FMath::RadiansToDegrees(FMath::Atan2(Intent.Y, Intent.X)));
		BodyYaw += FMath::FindDeltaAngleDegrees(BodyYaw, Target) * (1.f - FMath::Exp(-TurnSpeed * DeltaTime));
	}
	M->SetWorldRotation(FRotator(0.f, BodyYaw - 90.f, 0.f));

	// Фаза шага идёт по пройденному пути: так ноги не скользят по земле.
	const float Cycle = CycleLength + CycleLengthPerSpeed * Speed;
	if (bGrounded) Phase = FMath::Fmod(Phase + 2.f * PI * Speed / Cycle * DeltaTime, 2.f * PI);
	GaitWeight = FMath::FInterpConstantTo(GaitWeight, FMath::Clamp(Speed / 2.5f, 0.f, 1.f), DeltaTime, 4.f);
	AirWeight = FMath::FInterpConstantTo(AirWeight, bGrounded ? 0.f : 1.f, DeltaTime, 6.f);

	// Присед: насколько опустился таз, ноги сгибаются по IK.
	const float CrouchDrop = Crouch ? HipHeight * (1.f - Crouch->GetHeightFraction()) : 0.f;
	const float RestHipHeight = HipHeight - CrouchDrop;
	float CrouchHip, CrouchKnee;
	LegIK(RestHipHeight - AnkleHeight, CrouchHip, CrouchKnee);

	float HipL, KneeL, HipR, KneeR;
	LegAngles(Phase, CrouchHip, CrouchKnee, HipL, KneeL);
	LegAngles(Phase + PI, CrouchHip, CrouchKnee, HipR, KneeR);

	// На земле таз на высоте опорной (более длинной по вертикали) ноги; в воздухе опоры нет — таз остаётся на месте,
	// поджимаются ноги.
	// Опора переходит с ноги на ногу плавно (двойная опора — обе ноги несут тело): гладкий максимум, а не излом.
	const float DepthL = LegDepth(HipL, KneeL), DepthR = LegDepth(HipR, KneeR);
	const float Support = 0.5f * (DepthL + DepthR + FMath::Sqrt(FMath::Square(DepthL - DepthR) + FMath::Square(0.03f)));
	const float SupportHipHeight = AnkleHeight + Support;
	const float TargetPelvis = FMath::Lerp(SupportHipHeight, RestHipHeight, AirWeight);
	// Таз идёт к высоте, которую задают ноги, как масса на пружине ноги (полунеявно, мелкими шагами).
	if (PelvisHeight < 0.f) PelvisHeight = TargetPelvis;
	for (float Left = DeltaTime; Left > 0.f; Left -= 1.f / 240.f)
	{
		const float Step = FMath::Min(Left, 1.f / 240.f);
		PelvisSpeed += (LegOmega * LegOmega * (TargetPelvis - PelvisHeight) - 2.f * LegOmega * PelvisSpeed) * Step;
		PelvisHeight += PelvisSpeed * Step;
	}
	FTransform P = M->GetBoneTransformByName(Pelvis.Name, EBoneSpaces::ComponentSpace);
	P.SetLocation(Pelvis.Ref.GetLocation() + FVector(0, 0, (PelvisHeight - HipHeight) * 100.0));
	P.SetRotation(Pelvis.Ref.GetRotation());
	Apply(Pelvis.Name, P);
	if (CVarBodyDebug.GetValueOnGameThread() != 0)
		UE_LOG(LogTemp, Log, TEXT("Body: dt %.4f speed %.3f vz %.3f ground %d z %.2f phase %.2f gait %.2f air %.2f pelvis %.3f"),
			DeltaTime, Speed, Velocity.Z, bGrounded ? 1 : 0, GetOwner()->GetActorLocation().Z, Phase, GaitWeight, AirWeight, PelvisHeight);

	const float Breathe = FMath::Sin(Time * 1.6f) * (1.f - GaitWeight);
	const float Lean = -2.f * Breathe + 6.f * GaitWeight * FMath::Clamp(Speed / 6.f, 0.f, 1.f);
	SetRotation(Chest, FQuat(Side, FMath::DegreesToRadians(-Lean)) * Chest.Ref.GetRotation());

	SetLeg(ThighL, CalfL, FootL, HipL, KneeL);
	SetLeg(ThighR, CalfR, FootR, HipR, KneeR);
	SetArm(UpperL, ForeL, HandL, UpperRestL, ForeRestL, Phase + PI);
	SetArm(UpperR, ForeR, HandR, UpperRestR, ForeRestR, Phase);

	// Глаза — у головы этой позы (в исходной позе на высоте EyeHeight, чуть впереди кости головы).
	if (USceneComponent* Eye = EyeFollower.Get())
	{
		const FVector HeadCS = M->GetBoneTransformByName(Head.Name, EBoneSpaces::ComponentSpace).GetLocation();
		const FVector EyeCS = HeadCS + FVector(0, 6, EyeHeight * 100.0 - Head.Ref.GetLocation().Z);
		Eye->SetWorldLocation(M->GetComponentTransform().TransformPosition(EyeCS));
	}
}

// Угол вокруг поперечной оси: положительный — конечность вперёд.
void UBodyAnimator::LegAngles(float LegPhase, float CrouchHip, float CrouchKnee, float& Hip, float& Knee) const
{
	const float Swing = FMath::Sin(LegPhase);
	Hip = HipSwing * GaitWeight * Swing;
	// Колено сгибается, когда нога идёт вперёд (перенос).
	// Гладкая рампа max(0, cos): колено начинает сгибаться не рывком.
	const float C = FMath::Cos(LegPhase);
	Knee = GaitWeight * (8.f + KneeSwing * 0.5f * (C + FMath::Sqrt(C * C + 0.04f)));
	// В прыжке ноги поджаты.
	Hip = FMath::Lerp(Hip, 35.f, AirWeight) + CrouchHip;
	Knee = FMath::Lerp(Knee, 70.f, AirWeight) + CrouchKnee;
}

// Насколько лодыжка ниже тазобедренного сустава при данных углах бедра и колена.
float UBodyAnimator::LegDepth(float Hip, float Knee) const
{
	return ThighLength * FMath::Cos(FMath::DegreesToRadians(Hip)) + ShinLength * FMath::Cos(FMath::DegreesToRadians(Knee - Hip));
}

void UBodyAnimator::SetLeg(const FBone& Thigh, const FBone& Calf, const FBone& Foot, float Hip, float Knee)
{
	SetRotation(Thigh, FQuat(Side, FMath::DegreesToRadians(Hip)) * Thigh.Ref.GetRotation());
	SetRotation(Calf, FQuat(Side, FMath::DegreesToRadians(Hip - Knee)) * Calf.Ref.GetRotation());
	// Стопа остаётся примерно параллельна земле.
	SetRotation(Foot, Foot.Ref.GetRotation());
}

void UBodyAnimator::SetArm(const FBone& Upper, const FBone& Fore, const FBone& Hand, const FQuat& UpperRest, const FQuat& ForeRest, float ArmPhase)
{
	const float Swing = FMath::Lerp(ArmSwing * GaitWeight * FMath::Sin(ArmPhase), -20.f, AirWeight);
	const float Flex = 12.f + 25.f * GaitWeight + 40.f * AirWeight;
	const FQuat UpperQ = FQuat(Side, FMath::DegreesToRadians(Swing)) * UpperRest;
	const FQuat ForeQ = FQuat(Side, FMath::DegreesToRadians(Swing + Flex)) * ForeRest;
	SetRotation(Upper, UpperQ * Upper.Ref.GetRotation());
	SetRotation(Fore, ForeQ * Fore.Ref.GetRotation());
	SetRotation(Hand, ForeQ * Hand.Ref.GetRotation());
}

// Двухзвенная нога: углы бедра и колена, при которых от таза до лодыжки ровно Distance по вертикали.
void UBodyAnimator::LegIK(float Distance, float& Hip, float& Knee) const
{
	const float A = ThighLength, B = ShinLength;
	const float D = FMath::Clamp(Distance, FMath::Abs(A - B) + 0.01f, A + B - 0.001f);
	const float KneeInner = FMath::Acos(FMath::Clamp((A * A + B * B - D * D) / (2.f * A * B), -1.f, 1.f));
	Knee = 180.f - FMath::RadiansToDegrees(KneeInner);
	const float HipInner = FMath::Acos(FMath::Clamp((A * A + D * D - B * B) / (2.f * A * D), -1.f, 1.f));
	Hip = FMath::RadiansToDegrees(HipInner);
}

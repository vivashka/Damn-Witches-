#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BodyAnimator.generated.h"

class UPoseableMeshComponent;

// Процедурная анимация тела (перенос ProceduralBodyAnimator из Unity) на скелете манекена. Шаг зависит от скорости
// тела, руки в противофазе ногам, поза в прыжке, присед через IK ног. Тело поворачивается по направлению, куда
// персонаж хочет идти; суставы поворачиваются вокруг поперечной оси тела от исходной позы скелета.
// Таз стоит на опорной ноге: нижняя стопа всегда у земли (у низа капсулы), а не уходит в неё и не зависает.
// Длины звеньев ног берутся из скелета.
UCLASS(ClassGroup = (Body), meta = (BlueprintSpawnableComponent))
class UBodyAnimator : public UActorComponent
{
	GENERATED_BODY()
public:
	UBodyAnimator();

	// Длина полного цикла шага (два шага) при ходьбе, м; растёт со скоростью.
	UPROPERTY(EditAnywhere, Category = "Gait") float CycleLength = 1.3f;
	UPROPERTY(EditAnywhere, Category = "Gait") float CycleLengthPerSpeed = 0.25f;
	UPROPERTY(EditAnywhere, Category = "Gait") float HipSwing = 32.f;
	UPROPERTY(EditAnywhere, Category = "Gait") float KneeSwing = 60.f;
	UPROPERTY(EditAnywhere, Category = "Gait") float ArmSwing = 28.f;
	UPROPERTY(EditAnywhere, Category = "Gait") float TurnSpeed = 10.f;
	// Ноги — пружина под тазом (модель ходьбы «масса на пружине», Blickhan 1989): ω = √(k/m), у человека k ≈ 20 кН/м
	// при 77 кг → ~16 рад/с, демпфирование критическое. Таз — центр масс тела, его высота не меняется скачком.
	UPROPERTY(EditAnywhere, Category = "Gait") float LegOmega = 16.f;

	// ViewMesh — копия тела для своей камеры (без головы: камера внутри неё); поза у обоих одна.
	void Init(UPoseableMeshComponent* InMesh, UPoseableMeshComponent* InViewMesh = nullptr);
	// Глаза (камера от первого лица) — там, где голова в позе этого кадра: при приседе, наклоне, шаге. null — не вести.
	void SetEyeFollower(USceneComponent* Follower, float EyeHeightM) { EyeFollower = Follower; EyeHeight = EyeHeightM; }
	// Поворот тела (рыскание, град.), куда оно сейчас смотрит.
	float GetBodyYaw() const { return BodyYaw; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FBone
	{
		FName Name;
		int32 Index = INDEX_NONE;
		FTransform Ref; // исходная поза в пространстве компонента
	};
	FBone Bone(FName Name) const;
	void SetRotation(const FBone& B, const FQuat& Q);
	void LegAngles(float LegPhase, float CrouchHip, float CrouchKnee, float& Hip, float& Knee) const;
	float LegDepth(float Hip, float Knee) const;
	void LegIK(float Distance, float& Hip, float& Knee) const;
	void SetLeg(const FBone& Thigh, const FBone& Calf, const FBone& Foot, float Hip, float Knee);
	void SetArm(const FBone& Upper, const FBone& Fore, const FBone& Hand, const FQuat& UpperRest, const FQuat& ForeRest, float ArmPhase);

	TWeakObjectPtr<UPoseableMeshComponent> Mesh, ViewMesh;
	void Apply(FName BoneName, const FTransform& T);
	TArray<FTransform> RefComponent;
	TWeakObjectPtr<USceneComponent> EyeFollower;
	float EyeHeight = 1.72f;
	FBone Head;
	FBone Pelvis, Chest, ThighL, CalfL, FootL, ThighR, CalfR, FootR, UpperL, ForeL, HandL, UpperR, ForeR, HandR;
	FQuat UpperRestL, ForeRestL, UpperRestR, ForeRestR;
	FVector Side = FVector(1, 0, 0);     // поперечная ось тела в осях меша: поворот на +угол уводит конечность вперёд
	float ThighLength = 0.45f, ShinLength = 0.45f, AnkleHeight = 0.07f, HipHeight = 0.97f; // м
	float PelvisHeight = -1.f, PelvisSpeed = 0.f; // м, м/с — над низом тела
	float Phase = 0.f, GaitWeight = 0.f, AirWeight = 0.f, BodyYaw = 0.f, Time = 0.f;
	bool bReady = false;
};

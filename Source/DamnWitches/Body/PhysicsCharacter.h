#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "PhysicsCharacter.generated.h"

class UCapsuleComponent;
class UPoseableMeshComponent;
class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class UBodyMover;
class UBodyAnimator;
class UBodyStamina;
class UBodySprint;
class UBodyCrouch;
class UBodyDasher;
struct FInputActionValue;

// Физический персонаж (перенос Player из Unity): тело — физическое тело 77 кг ростом 1.85 м с замороженным вращением,
// его двигают ноги (UBodyMover) и всё, что действует на любое тело (гравитация, вода, удары). Анимация процедурная
// (UBodyAnimator). Части тела — сегменты по антропометрическим таблицам (Winter, «Biomechanics and Motor Control of
// Human Movement»), привязаны к костям и двигаются с анимацией; их видит вода — вытеснение, выталкивающая сила,
// сопротивление идут через них. Средняя плотность ≈ 990 кг/м³, как у человека.
// Камера — внутри головы: своё тело видно, как в Unity.
UCLASS()
class APhysicsCharacter : public APawn
{
	GENERATED_BODY()
public:
	APhysicsCharacter();

	UPROPERTY(EditAnywhere, Category = "Body") float Mass = 77.f;          // кг
	UPROPERTY(EditAnywhere, Category = "Body") float EyeHeight = 1.72f;    // м
	UPROPERTY(EditAnywhere, Category = "Body") float BodyRadius = 0.2f;    // м, тело для столкновений с миром

	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

protected:
	virtual void BeginPlay() override;

private:
	void BuildSegments();
	void UpdateHeight();
	void MoveInput(const FInputActionValue& Value);
	void LookInput(const FInputActionValue& Value);
	void JumpPressed();
	void SprintPressed() { bWantsSprint = true; }
	void SprintReleased() { bWantsSprint = false; }
	void CrouchPressed() { bWantsCrouch = true; }
	void CrouchReleased() { bWantsCrouch = false; }
	void DashPressed();
	FVector ForwardOnGround() const;
	FVector LastDirection = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere) TObjectPtr<UCapsuleComponent> Capsule;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPoseableMeshComponent> BodyMesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPoseableMeshComponent> ViewMesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UBodyMover> Mover;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UBodyAnimator> Animator;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UBodyStamina> Stamina;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UBodySprint> Sprint;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UBodyCrouch> Crouch;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UBodyDasher> Dasher;

	UPROPERTY(EditAnywhere, Category = "Input") TObjectPtr<UInputAction> MoveAction;
	UPROPERTY(EditAnywhere, Category = "Input") TObjectPtr<UInputAction> LookAction;
	UPROPERTY(EditAnywhere, Category = "Input") TObjectPtr<UInputAction> MouseLookAction;
	UPROPERTY(EditAnywhere, Category = "Input") TObjectPtr<UInputAction> JumpAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> SprintAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> CrouchAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> DashAction;
	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> BodyMapping;

	bool bWantsSprint = false;
	bool bWantsCrouch = false;
	float AppliedHalfHeight = 0.f;
	bool bThirdPerson = false;
	void UpdateView();
};

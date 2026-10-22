// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "DamnWitchesGameMode.generated.h"

/**
 *  Simple GameMode for a first person game
 */
UCLASS(abstract)
class ADamnWitchesGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ADamnWitchesGameMode();

	// Игрок — физический персонаж с процедурной анимацией (APhysicsCharacter), как в Unity. false — пешка из
	// настроек режима (шаблонный персонаж на CharacterMovement).
	UPROPERTY(EditAnywhere, Category = "Classes")
	bool bPhysicsCharacter = true;

	virtual UClass* GetDefaultPawnClassForController_Implementation(AController* InController) override;
};




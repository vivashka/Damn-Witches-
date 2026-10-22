// Copyright Epic Games, Inc. All Rights Reserved.

#include "DamnWitchesGameMode.h"
#include "Body/PhysicsCharacter.h"

ADamnWitchesGameMode::ADamnWitchesGameMode()
{
	// stub
}

UClass* ADamnWitchesGameMode::GetDefaultPawnClassForController_Implementation(AController* InController)
{
	return bPhysicsCharacter ? APhysicsCharacter::StaticClass() : Super::GetDefaultPawnClassForController_Implementation(InController);
}

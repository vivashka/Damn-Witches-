// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class DamnWitches : ModuleRules
{
	public DamnWitches(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { "PhysicsCore", "ProceduralMeshComponent" });

		PublicIncludePaths.AddRange(new string[] {
			"DamnWitches",
			"DamnWitches/Variant_Horror",
			"DamnWitches/Variant_Horror/UI",
			"DamnWitches/Variant_Shooter",
			"DamnWitches/Variant_Shooter/AI",
			"DamnWitches/Variant_Shooter/UI",
			"DamnWitches/Variant_Shooter/Weapons"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}

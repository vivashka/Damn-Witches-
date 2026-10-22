using System.IO;
using UnrealBuildTool;

public class SwampWater : ModuleRules
{
	public SwampWater(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore", "RHI", "Renderer", "Projects", "Landscape", "PhysicsCore"
		});

		// FViewInfo (прямоугольник вида в PrePostProcessPass) — приватный заголовок рендерера.
		string EnginePath = Path.GetFullPath(Target.RelativeEnginePath);
		PrivateIncludePaths.Add(Path.Combine(EnginePath, "Source/Runtime/Renderer/Private"));
		PrivateIncludePaths.Add(Path.Combine(EnginePath, "Source/Runtime/Renderer/Internal"));
	}
}

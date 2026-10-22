#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "Misc/Paths.h"

class FSwampWaterModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Шейдеры плагина — /Plugin/SwampWater (если менеджер плагинов не сопоставил их сам).
		const FString Virtual = TEXT("/Plugin/SwampWater");
		if (!AllShaderSourceDirectoryMappings().Contains(Virtual))
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SwampWater"));
			if (Plugin.IsValid()) AddShaderSourceDirectoryMapping(Virtual, FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
	}
};

IMPLEMENT_MODULE(FSwampWaterModule, SwampWater)

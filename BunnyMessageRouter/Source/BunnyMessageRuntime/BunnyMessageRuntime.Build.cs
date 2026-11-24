// BunnyMessageRouter. Messages delivered at the speed of a startled rabbit.

using UnrealBuildTool;

public class BunnyMessageRuntime : ModuleRules
{
	public BunnyMessageRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "GameplayTags" });
	}
}

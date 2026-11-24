// BunnyMessageRouter. Messages delivered at the speed of a startled rabbit.

using UnrealBuildTool;

public class BunnyMessageNodes : ModuleRules
{
	public BunnyMessageNodes(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "BlueprintGraph" });
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "KismetCompiler", "UnrealEd", "BunnyMessageRuntime" });
	}
}

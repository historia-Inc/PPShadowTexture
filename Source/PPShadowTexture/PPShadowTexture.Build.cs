using System.IO;
using UnrealBuildTool;

public class PPShadowTexture : ModuleRules
{
	public PPShadowTexture(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Projects",
			"RenderCore",
			"Renderer",
			"RHI"
		});

		// The shadow data lives in renderer-private structures (FSceneRenderer, FProjectedShadowInfo,
		// FVirtualShadowMapArray, FSceneTextures::UserSceneTextures). UE 5.8 and 5.9 only.
		PrivateIncludePaths.AddRange(new[]
		{
			Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"),
			Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal")
		});
	}
}

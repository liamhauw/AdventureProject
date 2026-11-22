using System.IO;
using UnrealBuildTool;

public class AdventureSuperResolution : ModuleRules
{
	public AdventureSuperResolution(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(new[] {
			"Core", "CoreUObject", "Engine", "NNE", "RHI", "RenderCore", "Renderer"
		});

		// ISpatialUpscaler and FViewInfo are renderer-private in UE 5.8.
		PrivateIncludePaths.AddRange(new[] {
			Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"),
			Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal")
		});
	}
}

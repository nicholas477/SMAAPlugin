// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SMAAPlugin : ModuleRules
{
	public SMAAPlugin(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
                "RHI",
                "RenderCore",
                "Renderer",
                "DeveloperSettings"
            }
		);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
                "Projects"
            }
		);

		PrivateIncludePaths.AddRange(
			new string[] {
				System.IO.Path.Combine(GetModuleDirectory("Renderer"), "Private"),
				System.IO.Path.Combine(GetModuleDirectory("Renderer"), "Internal")
			}
		);
	}
}
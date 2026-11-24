using System.IO;
using UnrealBuildTool;

public class Strybok : ModuleRules
{
	public Strybok(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HTTP",
			"Json",
		});

		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			// Adds the App Links intent filter and the GameActivity glue (see Strybok_UPL.xml).
			string PluginPath = Utils.MakePathRelativeTo(ModuleDirectory, Target.RelativeEnginePath);
			AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(PluginPath, "Strybok_UPL.xml"));
		}
	}
}

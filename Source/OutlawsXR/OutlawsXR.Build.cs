using UnrealBuildTool;
using System.IO;

// The headset's display refresh rate (OpenXR XR_FB_display_refresh_rate). UE 5.1's OpenXR plugin has no switch for it, so
// this small module asks for the extension itself. It loads at PostConfigInit: OpenXR creates its instance (and fixes
// the extension list) when the RHI starts, before the game modules load.
public class OutlawsXR : ModuleRules
{
	public OutlawsXR(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(ModuleDirectory);
		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
		PrivateDependencyModuleNames.AddRange(new string[] { "CoreUObject", "Engine", "HeadMountedDisplay", "AugmentedReality", "OpenXRHMD" });
		PrivateIncludePaths.Add(Path.Combine(Path.GetFullPath(Target.RelativeEnginePath), "Source/ThirdParty/OpenXR/include"));
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenXR");
	}
}

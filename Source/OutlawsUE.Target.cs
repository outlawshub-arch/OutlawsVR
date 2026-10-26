using UnrealBuildTool;
using System.Collections.Generic;

public class OutlawsUETarget : TargetRules
{
	public OutlawsUETarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V2;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_1;
		ExtraModuleNames.Add("OutlawsUE");
		ExtraModuleNames.Add("OutlawsRuntime");
		ExtraModuleNames.Add("OutlawsXR");
		// (REQ-SHIPLOG) no bUseLoggingInShipping: with the launcher engine it needs a unique build environment, which UBT refuses
		// for an installed engine, and its Shipping libraries have no logging anyway.  The Shipping log is the game's own
		// (Source/OutlawsRuntime/OutlawsShipLog.h).
	}
}

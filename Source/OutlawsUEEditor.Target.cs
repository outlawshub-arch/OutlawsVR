using UnrealBuildTool;
using System.Collections.Generic;

public class OutlawsUEEditorTarget : TargetRules
{
	public OutlawsUEEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V2;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_1;
		ExtraModuleNames.Add("OutlawsUE");
		ExtraModuleNames.Add("OutlawsRuntime");
		ExtraModuleNames.Add("OutlawsXR");
	}
}

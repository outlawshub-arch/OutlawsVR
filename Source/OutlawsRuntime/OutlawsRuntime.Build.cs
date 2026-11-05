using UnrealBuildTool;

// Runtime loader for Outlaws data: reads the player's own LAB archives (LVT/OBT/ITM/PCX/HDX/NWX/3DO)
// and builds the level at load time.  Editor-only code paths are guarded so a packaged exe carries it.
public class OutlawsRuntime : ModuleRules
{
	public OutlawsRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(ModuleDirectory);
		PrivateDefinitions.Add("WITH_OGGVORBIS=1");          // the engine's Vorbis decoder, for the Remastered taunts (.ogg)
		// the -OL test code (the test files, ShotTest, SmokeTest and their Tick dispatch): built in Development, Debug and the
		// editor, none of it in a Shipping build
		PublicDefinitions.Add("WITH_OUTLAWS_TESTS=" + (Target.Configuration == UnrealTargetConfiguration.Shipping ? "0" : "1"));
		// (REQ-Q2ROWHIDE, user 2 Nov) the release drop's builds: OL_RELEASE_BUILD=1 in the environment of the package build (off
		// by default, so night.sh's own builds for the user leave it off).  UBT keeps its makefile when only the environment
		// changes, so a release package build also gives UBT an argument the nightly's does not (-ubtargs="... -NoUBTMakefiles")
		PrivateDefinitions.Add("OL_RELEASE_BUILD=" + (System.Environment.GetEnvironmentVariable("OL_RELEASE_BUILD") == "1" ? "1" : "0"));
		// (REQ-AUTOUPDATE) the rising number the update check compares with latest.json: StoreVersion, the Android versionCode,
		// from the one place it is kept. OL_UPDATE_TEST_BUILD in the environment bakes a lower one for the real-app trigger test
		// (user 1 Nov: a build-time define, never a menu or dev switch; the versionCode stays, or Android refuses the update)
		int UpdateBuild = 0;
		string UpdateTest = System.Environment.GetEnvironmentVariable("OL_UPDATE_TEST_BUILD");
		if (string.IsNullOrEmpty(UpdateTest))
		{
			foreach (string IniLine in System.IO.File.ReadAllLines(System.IO.Path.Combine(ModuleDirectory, "..", "..", "Config", "DefaultEngine.ini")))
			{
				if (IniLine.StartsWith("StoreVersion=")) int.TryParse(IniLine.Substring("StoreVersion=".Length).Trim(), out UpdateBuild);
			}
		}
		else int.TryParse(UpdateTest, out UpdateBuild);
		PrivateDefinitions.Add("OL_UPDATE_BUILD=" + UpdateBuild);
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "Slate", "SlateCore", "RenderCore", "RHI", "ProceduralMeshComponent", "HeadMountedDisplay", "AudioMixer", "MediaAssets", "Media", "Json" });
		PrivateDependencyModuleNames.Add("Sockets");          // co-op: the machine's local network address for the lobby
		PrivateDependencyModuleNames.Add("HTTP");             // co-op (R8): the host's internet address for its lobby
		PrivateDependencyModuleNames.Add("OutlawsXR");        // the headset's refresh rate (the Graphics page, the Quest 2 profile)
		PrivateDependencyModuleNames.Add("ImageCore");        // the shipped held guns' texture templates (FImage)
		PrivateDependencyModuleNames.Add("ImageWrapper");     // PNG files: the bug snaps (OutlawsBugSnap) and the tests' pictures
		PrivateDependencyModuleNames.Add("GeometryCore");     // the runtime held guns: Delaunay (OutlawsGunBuilder)
		// co-op voice chat (user 2026-10-02): the engine's microphone capture and Opus codec (its header needs SignalProcessing)
		PrivateDependencyModuleNames.AddRange(new string[] { "Voice", "SignalProcessing" });
		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			PrivateDependencyModuleNames.Add("AndroidPermission");   // the Quest's RECORD_AUDIO asked for at runtime
			PrivateDependencyModuleNames.Add("Launch");              // (REQ-AUTOUPDATE) FJavaWrapper, the installer's Java side
			AdditionalPropertiesForReceipt.Add("AndroidPlugin", System.IO.Path.Combine(ModuleDirectory, "OutlawsUpdate_UPL.xml"));
		}
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// the music on a PC: the install's flac decoded by the engine's libsndfile (Audio::ConvertAudioToWav), which a
			// packaged game does not stage on its own (LGPL: its notice goes in the PC zip, tools/olue/package_pc.cmd)
			RuntimeDependencies.Add("$(EngineDir)/Binaries/ThirdParty/libsndfile/Win64/libsndfile-1.dll");
			PublicSystemLibraries.Add("dxgi.lib");           // (BUG-RESPAWNCRASH, -OLMemSoak) the process's video memory in the OLMEM lines
		}
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd" });
		}
	}
}

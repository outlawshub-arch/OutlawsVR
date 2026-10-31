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

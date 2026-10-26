// Automation tests: the port's -maptest, -patchtest, -savetest, -exittest, -edittest and
// screenshot switches as Unreal automation tests, under "DoomUE" in the Session Frontend's
// Automation tab, or headless:
//
//   UnrealEditor-Cmd.exe DoomUE.uproject -ExecCmds="Automation RunTests DoomUE; Quit" -unattended -nopause -NullRHI
//
// (Tools/run_tests.bat runs them, the DoomUE.Render screenshots too.) Map and node tests read
// WADs directly; the others play the game in PIE on /Engine/Maps/Entry, the way the switches did in
// a -game run, and the screenshot tests compare against the approved images in Test/Screenshots.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "AutomationBlueprintFunctionLibrary.h"
#include "Editor.h"
#include "Kismet/GameplayStatics.h"
#include "DoomWad.h"
#include "DoomMap.h"
#include "DoomInfo.h"
#include "DoomWorld.h"
#include "DoomGameMode.h"
#include "DoomPlayer.h"
#include "DoomPlayerState.h"
#include "DoomMonster.h"
#include "DoomGraphics.h"
#include "DoomMapInfo.h"
#include "DoomContentBuilder.h"
#include "DoomLevel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DoomTest
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	FString WadPath(const FString& Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("WADs") / Name);
	}

	FString TestWad(const FString& Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Tests/wads") / Name);
	}

	UWorld* PlayWorld()
	{
		return GEditor ? GEditor->PlayWorld.Get() : nullptr;
	}

	UDoomWorld* Doom()
	{
		UWorld* W = PlayWorld();
		return W ? W->GetSubsystem<UDoomWorld>() : nullptr;
	}

	/** Runs Step each frame until it returns true; an error after Timeout seconds. */
	class FStep : public IAutomationLatentCommand
	{
	public:
		FStep(FAutomationTestBase* InTest, const FString& InWhat, TFunction<bool()> InStep, float InTimeout = 60.f)
			: Test(InTest), What(InWhat), Step(MoveTemp(InStep)), Timeout(InTimeout) {}

		virtual bool Update() override
		{
			// timed from this step's first frame (the commands are all created when the test starts)
			if (Started == 0)
			{
				Started = FPlatformTime::Seconds();
			}
			if (Step())
			{
				return true;
			}
			if (FPlatformTime::Seconds() - Started > Timeout)
			{
				Test->AddError(FString::Printf(TEXT("Timed out after %.0f s: %s"), Timeout, *What));
				return true;
			}
			return false;
		}

	private:
		FAutomationTestBase* Test;
		FString What;
		TFunction<bool()> Step;
		float Timeout;
		double Started = 0;
	};

	/** Waits Seconds of real time. */
	TFunction<bool()> Wait(float Seconds)
	{
		TSharedRef<double> Until = MakeShared<double>(0);
		return [Until, Seconds]()
		{
			if (*Until == 0)
			{
				*Until = FPlatformTime::Seconds() + Seconds;
			}
			return FPlatformTime::Seconds() >= *Until;
		};
	}

	/** Waits until Count more frames have been rendered (Lumen settles per frame, not per second, so a
	 *  fixed time lets a fast GPU settle further than a slow one); logs how long they took. */
	TFunction<bool()> Frames(FAutomationTestBase* Test, uint64 Count)
	{
		TSharedRef<uint64> Until = MakeShared<uint64>(0);
		TSharedRef<double> Started = MakeShared<double>(0);
		return [Test, Count, Until, Started]()
		{
			if (*Until == 0)
			{
				*Until = GFrameCounter + Count;
				*Started = FPlatformTime::Seconds();
			}
			if (GFrameCounter < *Until)
			{
				return false;
			}
			const double Secs = FPlatformTime::Seconds() - *Started;
			Test->AddInfo(FString::Printf(TEXT("%llu frames in %.1f s (%.0f fps)"), Count, Secs, Count / FMath::Max(Secs, 0.001)));
			return true;
		};
	}

	/** Opens the empty entry map and plays it: the Doom game mode loads the WADs and parks the player.
	 *  In the editor AutomationOpenMap queues both the map load and the PIE start. */
	void StartGame(FAutomationTestBase* Test)
	{
		AutomationOpenMap(TEXT("/Engine/Maps/Entry"));
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("PIE with the WADs loaded and a player"), []()
		{
			UDoomWorld* W = Doom();
			return W && W->GetWad() && W->GetPlayer();
		}));
	}

	/** Ends PIE; the info tables are global, so patched ones go back to the defaults first. */
	void EndGame(FAutomationTestBase* Test)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("reset the tables"), []()
		{
			if (UDoomWorld* W = Doom())
			{
				W->InitWads();
			}
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	}

	/** Loads WADs and patches in the running game, then runs Check against the result. */
	void CheckPatches(FAutomationTestBase* Test, const FDoomWadSetup& Setup, TFunction<void(UDoomWorld*)> Check)
	{
		StartGame(Test);
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("load the patches"), [Test, Setup, Check]()
		{
			UDoomWorld* W = Doom();
			if (Test->TestTrue(TEXT("WADs and patches load"), W->InitWads(&Setup)))
			{
				Check(W);
			}
			return true;
		}));
		EndGame(Test);
	}

	/** Loads a map in the running game. */
	void LoadMap(FAutomationTestBase* Test, const FString& Map, TOptional<bool> bModern = {})
	{
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("load ") + Map, [Test, Map, bModern]()
		{
			UDoomWorld* W = Doom();
			if (bModern.IsSet())
			{
				W->SetModern(bModern.GetValue());
			}
			Test->TestTrue(FString::Printf(TEXT("%s loads"), *Map), W->LoadMap(Map, true));
			return true;
		}));
	}
}

using namespace DoomTest;

// ---- maps and node formats (-maptest) ------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomMapE1M1Test, "DoomUE.Map.E1M1", DoomTest::Flags)
bool FDoomMapE1M1Test::RunTest(const FString& Parameters)
{
	FDoomWad Wad;
	if (!TestTrue(TEXT("freedoom1.wad loads"), Wad.AddFile(WadPath(TEXT("freedoom1.wad")))))
	{
		return false;
	}
	Wad.Finalize();
	FDoomMap Map;
	if (!TestTrue(TEXT("E1M1 loads"), Map.Load(Wad, TEXT("E1M1"))))
	{
		return false;
	}
	// the Pascal port's reference: 682 subsectors, 6,712,683 square units (6,712,686 here, rounding)
	TestEqual(TEXT("nodes"), Map.NodesFormat, FString(TEXT("vanilla")));
	TestEqual(TEXT("subsectors"), Map.Subsectors.Num(), 682);
	TestNearlyEqual(TEXT("subsector area"), Map.SubsectorArea(), 6712686.0, 2.0);
	TestEqual(TEXT("sectors"), Map.Sectors.Num(), 182);
	TestEqual(TEXT("lines"), Map.Lines.Num(), 1175);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomMapNodeFormatsTest, "DoomUE.Map.NodeFormats", DoomTest::Flags)
bool FDoomMapNodeFormatsTest::RunTest(const FString& Parameters)
{
	// E1M1 rebuilt with every node format the loader reads, and converted to UDMF: all give the
	// vanilla subsectors (glBSP v1 stores 16-bit vertices, so its area differs slightly)
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *TestWad(TEXT("e1m1_*.wad")), true, false);
	TestTrue(TEXT("test WADs found in Tests/wads"), Files.Num() >= 10);
	for (const FString& File : Files)
	{
		FDoomWad Wad;
		Wad.AddFile(WadPath(TEXT("freedoom1.wad")));
		Wad.AddFile(TestWad(File));
		Wad.Finalize();
		FDoomMap Map;
		if (!TestTrue(File + TEXT(" loads"), Map.Load(Wad, TEXT("E1M1"))))
		{
			continue;
		}
		const double Expected = File.Contains(TEXT("gl_v1")) ? 6709159.0 : 6712686.0;
		TestEqual(File + TEXT(" subsectors"), Map.Subsectors.Num(), 682);
		TestNearlyEqual(File + TEXT(" area"), Map.SubsectorArea(), Expected, 2.0);
		AddInfo(FString::Printf(TEXT("%s: %s nodes"), *File, *Map.NodesFormat));
	}
	return true;
}

// ---- patches (-patchtest) ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomPatchDehackedTest, "DoomUE.Patches.DeHackEd", DoomTest::Flags)
bool FDoomPatchDehackedTest::RunTest(const FString& Parameters)
{
	FDoomWadSetup Setup;
	Setup.DehFiles = { TestWad(TEXT("test.deh")) };
	CheckPatches(this, Setup, [this](UDoomWorld* W)
	{
		const FDoomThingDef* Imp = DoomInfo::FindThing(3001);
		const FDoomProjectileDef* Ball = DoomInfo::FindProjectile(TEXT("ImpBall"));
		const FDoomRules& R = DoomInfo::Rules();
		TestEqual(TEXT("Thing 12 (Imp) hit points"), Imp->Health, 1);
		TestEqual(TEXT("Thing 12 (Imp) width"), Imp->Radius, 20.f);
		TestEqual(TEXT("Misc initial health"), R.InitialHealth, 50);
		TestEqual(TEXT("Misc initial bullets"), R.InitialBullets, 20);
		TestEqual(TEXT("Ammo 0 max"), R.MaxAmmo[0], 100);
		TestEqual(TEXT("Ammo 0 per clip"), R.ClipAmmo[0], 7);
		TestEqual(TEXT("Thing 32 (Imp Fireball) speed"), Ball->Speed, 20.f);
		TestEqual(TEXT("Thing 32 (Imp Fireball) damage"), Ball->DamageMul, 1);
		TestEqual(TEXT("[PARS] E1M1"), DoomInfo::ParTime(TEXT("E1M1")), 999);
		TestEqual(TEXT("[STRINGS] GOTCLIP"), DoomInfo::PickupMessage(EPickup::Clip), FString(TEXT("Picked up a DEH clip.Really.")));
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomPatchTextTest, "DoomUE.Patches.TextBlocks", DoomTest::Flags)
bool FDoomPatchTextTest::RunTest(const FString& Parameters)
{
	// Text blocks: sprite and sound renames, and a string replaced by matching its text
	FDoomWadSetup Setup;
	Setup.DehFiles = { TestWad(TEXT("text.deh")) };
	CheckPatches(this, Setup, [this](UDoomWorld* W)
	{
		TestEqual(TEXT("imps use the baron sprite"), DoomInfo::SpriteName(TEXT("TROO")), FString(TEXT("BOSS")));
		TestEqual(TEXT("health bonuses look like armor bonuses"), DoomInfo::SpriteName(TEXT("BON1")), FString(TEXT("BON2")));
		TestEqual(TEXT("the pistol sounds like the shotgun"), DoomInfo::SoundName(TEXT("PISTOL")), FString(TEXT("SHOTGN")));
		TestEqual(TEXT("clip message"), DoomInfo::PickupMessage(EPickup::Clip), FString(TEXT("Got bullets!")));
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomPatchBoomTest, "DoomUE.Patches.BoomLumps", DoomTest::Flags)
bool FDoomPatchBoomTest::RunTest(const FString& Parameters)
{
	// SWITCHES and ANIMATED lumps replace the built-in lists
	FDoomWadSetup Setup;
	Setup.PWads = { TestWad(TEXT("boom.wad")) };
	CheckPatches(this, Setup, [this](UDoomWorld* W)
	{
		FDoomGraphics* G = W->GetGraphics();
		TestEqual(TEXT("SWITCHES pairs SW1BRN1 with STARTAN3"), G->SwitchPartner(TEXT("SW1BRN1")).ToString(), FString(TEXT("STARTAN3")));
		TestEqual(TEXT("SWITCHES keeps SW1GRAY"), G->SwitchPartner(TEXT("SW1GRAY")).ToString(), FString(TEXT("SW2GRAY")));
		TestTrue(TEXT("SWITCHES drops SW1HOT"), G->SwitchPartner(TEXT("SW1HOT")).IsNone());
		TestTrue(TEXT("ANIMATED keeps NUKAGE1"), G->IsAnimated(TEXT("NUKAGE1"), true));
		TestFalse(TEXT("ANIMATED drops FWATER1"), G->IsAnimated(TEXT("FWATER1"), true));
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomPatchUmapinfoTest, "DoomUE.Patches.UMAPINFO", DoomTest::Flags)
bool FDoomPatchUmapinfoTest::RunTest(const FString& Parameters)
{
	FDoomWadSetup Setup;
	Setup.PWads = { TestWad(TEXT("umapinfo.wad")) };
	CheckPatches(this, Setup, [this](UDoomWorld* W)
	{
		const FDoomMapInfo* Info = W->MapInfo(TEXT("E1M1"));
		if (!TestNotNull(TEXT("E1M1 has an entry"), Info))
		{
			return;
		}
		TestEqual(TEXT("E1M1 title"), W->LevelTitle(TEXT("E1M1")), FString(TEXT("T1: Test Hangar")));
		TestEqual(TEXT("E1M1 par time"), DoomInfo::ParTime(TEXT("E1M1")), 123);
		TestEqual(TEXT("E1M1 music"), Info->Music, FString(TEXT("D_E1M5")));
		TestEqual(TEXT("E1M1 sky"), Info->Sky, FString(TEXT("SKY2")));
		TestEqual(TEXT("E1M1 next"), Info->Next, FString(TEXT("E1M3")));
		TestEqual(TEXT("E1M3 title"), W->LevelTitle(TEXT("E1M3")), FString(TEXT("Third Test")));
	});
	return true;
}

// ---- game (-savetest, -exittest) -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomSaveLoadTest, "DoomUE.Game.SaveLoad", DoomTest::Flags)
bool FDoomSaveLoadTest::RunTest(const FString& Parameters)
{
	// a door in motion, corpses, awake monsters with targets, a rocket and a homing tracer in flight:
	// save, scramble the world, load, and the state fingerprint must match the saved one
	StartGame(this);
	LoadMap(this, TEXT("E1M1"));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("settle"), Wait(1.f)));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("save, scramble, load"), [this]()
	{
		FString Report;
		const bool bPass = Doom()->RunSaveTest(&Report);
		AddInfo(Report);
		TestTrue(TEXT("the loaded state matches the saved one"), bPass);
		return true;
	}));
	EndGame(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomExitTest, "DoomUE.Game.ExitToNextMap", DoomTest::Flags)
bool FDoomExitTest::RunTest(const FString& Parameters)
{
	// kill two monsters as the player, exit, sit in the intermission, go on: the next map loads in
	// the background; totals and credit start again, the inventory carries over
	struct FState
	{
		int32 Kills = 0, Credit = 0;
		bool bLoadingInBackground = false;
	};
	TSharedRef<FState> S = MakeShared<FState>();
	StartGame(this);
	LoadMap(this, TEXT("E1M1"));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("kill two monsters and exit"), [this, S]()
	{
		UDoomWorld* W = Doom();
		ADoomPlayer* P = W->GetPlayer();
		int32 Killed = 0;
		for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
		{
			if (Killed < 2 && M.IsValid() && !M->IsDead())
			{
				UGameplayStatics::ApplyDamage(M.Get(), 10000.f, P->GetController(), P, nullptr);
				++Killed;
			}
		}
		P->Inv().Ammo[1] = 17;
		S->Kills = W->Flow().Kills;
		S->Credit = P->GetDoomPlayerState()->LevelKills;
		PlayWorld()->GetAuthGameMode<ADoomGameMode>()->ExitLevel(false);
		TestEqual(TEXT("game state kills"), S->Kills, 2);
		TestEqual(TEXT("player state credit"), S->Credit, 2);
		TestEqual(TEXT("in the intermission"), (int32)W->GetState(), (int32)EDoomGameState::Intermission);
		TestEqual(TEXT("next map"), W->Flow().PendingMap, FString(TEXT("E1M2")));
		return true;
	}));
	// Doom's own intermission waits a second before it can be left
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("the intermission counts 35 tics"), []() { return Doom()->Flow().IntermissionTic >= 35; }));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("leave the intermission"), [this, S]()
	{
		UDoomWorld* W = Doom();
		PlayWorld()->GetAuthGameMode<ADoomGameMode>()->FinishIntermission();
		S->bLoadingInBackground = W->IsLoadingMap();
		TestTrue(TEXT("E1M2 loads in the background"), S->bLoadingInBackground);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("E1M2 loaded"), []() { return !Doom()->IsLoadingMap(); }));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("check E1M2"), [this]()
	{
		UDoomWorld* W = Doom();
		ADoomPlayer* P = W->GetPlayer();
		TestEqual(TEXT("map"), W->GetMapName(), FString(TEXT("E1M2")));
		TestEqual(TEXT("playing"), (int32)W->GetState(), (int32)EDoomGameState::Playing);
		TestEqual(TEXT("kills start again"), W->Flow().Kills, 0);
		TestEqual(TEXT("credit starts again"), P->GetDoomPlayerState()->LevelKills, 0);
		TestEqual(TEXT("shells carried over"), P->GetDoomPlayerState()->Inv.Ammo[1], 17);
		return true;
	}));
	EndGame(this);
	return true;
}

// ---- editor (-edittest) --------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomEditMapTest, "DoomUE.Editor.EditImportedMap", DoomTest::Flags)
bool FDoomEditMapTest::RunTest(const FString& Parameters)
{
	// opens /Game/Doom/Maps/E1M1 in the editor, raises the start sector with a sector edit, turns an
	// imp into a baron and saves /Game/Doom/Tests/E1M1_Edit (it replaces the level open in the editor)
	TestTrue(TEXT("the preview rebuilds, the edits apply and the level saves"), DoomContent::EditTest(TEXT("E1M1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomDrawSectorTest, "DoomUE.Editor.DrawSector", DoomTest::Flags)
bool FDoomDrawSectorTest::RunTest(const FString& Parameters)
{
	// Doom > Draw Sectors in /Game/Doom/Maps/E1M1: a room in the void and one in the start room, undo
	// and redo, a sector edit on a drawn sector; saves /Game/Doom/Tests/E1M1_Draw
	TestTrue(TEXT("the drawn rooms apply, undo and redo work, and a sector edit works on them"), DoomContent::DrawTest(TEXT("E1M1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomWadTilesTest, "DoomUE.Editor.WadTiles", DoomTest::Flags)
bool FDoomWadTilesTest::RunTest(const FString& Parameters)
{
	// a map played straight from the WAD (no imported level) uses its baked tile set (-baketiles),
	// which is how packaged builds get distance fields for it
	if (!TestTrue(TEXT("E1M2's tile set bakes"), DoomContent::BakeWadTiles(TEXT("E1M2"))))
	{
		return false;
	}
	StartGame(this);
	LoadMap(this, TEXT("E1M2"), true);
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("check the tiles"), [this]()
	{
		const ADoomLevel* Level = Doom()->GetLevel();
		if (TestNotNull(TEXT("level"), Level))
		{
			TestTrue(TEXT("the static chunk is drawn by the saved tile set"), Level->UsesSavedTiles());
			TestTrue(TEXT("tiles"), Level->NumBakedTiles() > 0);
			AddInfo(FString::Printf(TEXT("E1M2: %d saved tiles"), Level->NumBakedTiles()));
		}
		return true;
	}));
	// the new meshes' distance fields build in the background: finish them, or they slow the next tests
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("distance fields"), []()
	{
		UAutomationBlueprintFunctionLibrary::FinishLoadingBeforeScreenshot();
		return true;
	}, 600.f));
	EndGame(this);
	return true;
}

// ---- screenshots (-autotest) ---------------------------------------------------------------------

namespace DoomTest
{
	/** Rendered frames the modern renderer gets before its screenshot: 600 frames is 10 s at 60 fps,
	 *  well past the 6 s the test used to wait on the owner's machine. */
	constexpr uint64 ModernSettleFrames = 600;

	/** E1M1's start in one renderer, compared against the approved image (needs a real RHI). */
	void Screenshot(FAutomationTestBase* Test, const FString& Name, bool bModern)
	{
		if (!FApp::CanEverRender())
		{
			Test->AddWarning(TEXT("Skipped: screenshots need rendering (this run uses -NullRHI)"));
			return;
		}
		StartGame(Test);
		// Doom's clock stops before the map loads, and exactly 35 tics run: monsters, animations and
		// lights are in the same state every run, while the engine keeps rendering
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("stop the clock"), []()
		{
			PlayWorld()->GetAuthGameMode<ADoomGameMode>()->bClockStopped = true;
			return true;
		}));
		LoadMap(Test, TEXT("E1M1"), bModern);
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("run 35 tics"), []()
		{
			for (int32 i = 0; i < 35; ++i)
			{
				Doom()->RunTic();
			}
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("shaders and distance fields"), []()
		{
			UAutomationBlueprintFunctionLibrary::FinishLoadingBeforeScreenshot();
			return true;
		}, 600.f));
		// Lumen gathers its lighting over frames: the same number of frames on every GPU, so a slow card
		// captures as settled a picture as a fast one (the classic renderer only needs a frame or two)
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("lighting settles"), bModern ? Frames(Test, ModernSettleFrames) : Wait(1.f), 600.f));
		TSharedRef<bool> bCompared = MakeShared<bool>(false);
		TSharedRef<FDelegateHandle> Handle = MakeShared<FDelegateHandle>();
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("take ") + Name, [Test, Name, bCompared, Handle]()
		{
			*Handle = FAutomationTestFramework::Get().OnScreenshotTakenAndCompared.AddLambda([bCompared]() { *bCompared = true; });
			FAutomationScreenshotOptions Options = UAutomationBlueprintFunctionLibrary::GetDefaultScreenshotOptionsForGameplay(EComparisonTolerance::Low, 0.f);
			Options.bDisableTonemapping = false; // the renderers set their own tone curve
			Test->TestTrue(TEXT("screenshot requested"), UAutomationBlueprintFunctionLibrary::TakeAutomationScreenshotInternal(PlayWorld(), Name, TEXT("E1M1 start"), Options));
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("compare ") + Name, [bCompared, Handle]()
		{
			if (*bCompared)
			{
				FAutomationTestFramework::Get().OnScreenshotTakenAndCompared.Remove(*Handle);
			}
			return *bCompared;
		}, 120.f));
		EndGame(Test);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomScreenshotClassicTest, "DoomUE.Render.E1M1Classic", DoomTest::Flags)
bool FDoomScreenshotClassicTest::RunTest(const FString& Parameters)
{
	Screenshot(this, TEXT("DoomUE_E1M1_Classic"), false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomScreenshotModernTest, "DoomUE.Render.E1M1Modern", DoomTest::Flags)
bool FDoomScreenshotModernTest::RunTest(const FString& Parameters)
{
	Screenshot(this, TEXT("DoomUE_E1M1_Modern"), true);
	return true;
}

#endif

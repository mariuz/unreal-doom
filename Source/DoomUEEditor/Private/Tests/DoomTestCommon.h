// Shared helpers for the DoomUE automation tests: play the game in PIE on /Engine/Maps/Entry, run
// latent steps until they report done, load maps.
#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "DoomWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DoomTest
{
	inline constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	inline FString WadPath(const FString& Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("WADs") / Name);
	}

	inline FString TestWad(const FString& Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Tests/wads") / Name);
	}

	inline UWorld* PlayWorld()
	{
		return GEditor ? GEditor->PlayWorld.Get() : nullptr;
	}

	inline UDoomWorld* Doom()
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
	inline TFunction<bool()> Wait(float Seconds)
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
	inline TFunction<bool()> Frames(FAutomationTestBase* Test, uint64 Count)
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
	/** Swallows the real mouse while a game test runs: the pointer moving over the editor window turned
	 *  the camera mid-screenshot (mouse look) and hovered menu rows mid-key-test. */
	class FMouseBlocker : public IInputProcessor
	{
	public:
		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
		virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override { return true; }
		virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override { return true; }
		virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override { return true; }
		virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override { return true; }
		virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override { return true; }
		virtual const TCHAR* GetDebugName() const override { return TEXT("DoomTestMouseBlocker"); }
	};

	inline TSharedPtr<FMouseBlocker>& MouseBlocker()
	{
		static TSharedPtr<FMouseBlocker> Blocker;
		return Blocker;
	}

	inline void BlockMouse(bool bBlock)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		TSharedPtr<FMouseBlocker>& B = MouseBlocker();
		if (bBlock && !B.IsValid())
		{
			B = MakeShared<FMouseBlocker>();
			FSlateApplication::Get().RegisterInputPreProcessor(B, 0);
		}
		else if (!bBlock && B.IsValid())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(B);
			B.Reset();
		}
	}

	inline void StartGame(FAutomationTestBase* Test)
	{
		BlockMouse(true);
		AutomationOpenMap(TEXT("/Engine/Maps/Entry"));
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("PIE with the WADs loaded and a player"), []()
		{
			UDoomWorld* W = Doom();
			return W && W->GetWad() && W->GetPlayer();
		}));
	}

	/** Ends PIE; the info tables are global, so patched ones go back to the defaults first. */
	inline void EndGame(FAutomationTestBase* Test)
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
		ADD_LATENT_AUTOMATION_COMMAND(FStep(Test, TEXT("the mouse back"), []()
		{
			BlockMouse(false);
			return true;
		}));
	}

	/** Loads WADs and patches in the running game, then runs Check against the result. */
	inline void CheckPatches(FAutomationTestBase* Test, const FDoomWadSetup& Setup, TFunction<void(UDoomWorld*)> Check)
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
	inline void LoadMap(FAutomationTestBase* Test, const FString& Map, TOptional<bool> bModern = {})
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

#endif

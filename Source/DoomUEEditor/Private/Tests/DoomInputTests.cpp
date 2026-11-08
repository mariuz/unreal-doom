// Key presses the way a player makes them: Slate key events into the PIE viewport, through CommonUI,
// the game viewport, the player controller and Enhanced Input. They cover what the -playtest can't
// press in its unfocused -game window: Esc and the menu keys, the prompts, the automap keys, moving,
// firing and the weapon keys.
//
// Each press waits for the screen to settle first (the CommonUI menu screen up exactly when the game
// is in the menu), then waits for what the key should do, as a player would look before pressing on.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SViewport.h"
#include "GameFramework/PlayerController.h"
#include "DoomGameMode.h"
#include "DoomPlayer.h"
#include "DoomPlayerController.h"
#include "DoomHUD.h"
#include "DoomMenu.h"
#include "DoomSettings.h"
#include "DoomUE.h"
#include "DoomTestCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DoomKeyTest
{
	UDoomWorld* World()
	{
		return DoomTest::Doom();
	}

	APlayerController* Controller()
	{
		UDoomWorld* W = World();
		ADoomPlayer* P = W ? W->GetPlayer() : nullptr;
		return P ? Cast<APlayerController>(P->GetController()) : nullptr;
	}

	const ADoomHUD* Hud()
	{
		APlayerController* PC = Controller();
		return PC ? Cast<ADoomHUD>(PC->GetHUD()) : nullptr;
	}

	/** Keyboard focus to the PIE viewport, as when the player clicks into it. */
	void FocusGame()
	{
		UGameViewportClient* VC = DoomTest::PlayWorld() ? DoomTest::PlayWorld()->GetGameViewport() : nullptr;
		TSharedPtr<SViewport> Widget = VC ? VC->GetGameViewportWidget() : nullptr;
		if (Widget.IsValid())
		{
			FSlateApplication::Get().SetAllUserFocus(Widget, EFocusCause::Mouse);
		}
	}

	/** The CommonUI menu screen is up exactly when the game is in the menu. */
	bool Settled()
	{
		UDoomWorld* W = World();
		APlayerController* PC = Controller();
		return W && PC && (W->GetState() == EDoomGameState::Menu) == ADoomPlayerController::IsMenuScreenShown(PC);
	}

	/** The game state, the menu and the focused widget, for the test report. */
	FString Describe()
	{
		const UDoomWorld* W = World();
		const TSharedPtr<SWidget> Focus = FSlateApplication::Get().GetUserFocusedWidget(0);
		return FString::Printf(TEXT("state %d, page %d, row %d, automap %d, focus %s"), W ? (int32)W->GetState() : -1,
			W ? (int32)W->Menu.GetPage() : -1, W ? W->Menu.GetItem() : -1, W ? (int32)W->IsAutomapOpen() : -1,
			Focus.IsValid() ? *Focus->GetTypeAsString() : TEXT("none"));
	}

	/** One key press: waits for the screen to settle, presses Key, holds it HoldFrames frames, releases
	 *  it and waits until Done says it did its work. Pressed again (up to MaxPresses) while Done is false,
	 *  for moving to a row; an error naming What if it never is. Before runs just before the first press. */
	class FPressKey : public IAutomationLatentCommand
	{
	public:
		FPressKey(FAutomationTestBase* InTest, const FKey& InKey, const FString& InWhat, TFunction<bool()> InDone, uint64 InHold,
			int32 InMaxPresses, TFunction<void()> InBefore)
			: Test(InTest), Key(InKey), What(InWhat), Done(MoveTemp(InDone)), Before(MoveTemp(InBefore)), Hold(InHold), MaxPresses(InMaxPresses) {}

		virtual bool Update() override
		{
			const double Now = FPlatformTime::Seconds();
			if (Started == 0)
			{
				Started = Now;
			}
			if (Now - Started > 10.0 + MaxPresses)
			{
				Test->AddError(FString::Printf(TEXT("%s: timed out (%s)"), *What, *Describe()));
				return true;
			}
			switch (Phase)
			{
			case EPhase::Settle:
				if (!Settled())
				{
					return false;
				}
				if (SettledFrame == 0)
				{
					SettledFrame = GFrameCounter;
				}
				if (GFrameCounter < SettledFrame + 3)
				{
					return false;
				}
				// pressing until something holds (moving to a row): no press if it already does
				if (Presses == 0 && MaxPresses > 1 && Done && Done())
				{
					Test->AddInfo(FString::Printf(TEXT("%s (%s x0): %s"), *What, *Key.ToString(), *Describe()));
					return true;
				}
				if (Presses == 0 && Before)
				{
					Before();
				}
				FSlateApplication::Get().ProcessKeyDownEvent(Event());
				++Presses;
				PressedFrame = GFrameCounter;
				Phase = EPhase::Hold;
				return false;
			case EPhase::Hold:
				if (GFrameCounter < PressedFrame + Hold)
				{
					return false;
				}
				FSlateApplication::Get().ProcessKeyUpEvent(Event());
				Released = Now;
				ReleasedFrame = GFrameCounter;
				Phase = EPhase::Check;
				return false;
			case EPhase::Check:
				if (GFrameCounter < ReleasedFrame + 2)
				{
					return false;
				}
				if (!Done || Done())
				{
					Test->AddInfo(FString::Printf(TEXT("%s (%s x%d): %s"), *What, *Key.ToString(), Presses, *Describe()));
					return true;
				}
				if (Now - Released < 1.5)
				{
					return false;
				}
				if (Presses < MaxPresses)
				{
					Phase = EPhase::Settle;
					SettledFrame = 0;
					return false;
				}
				Test->AddError(FString::Printf(TEXT("%s: not after %d presses of %s (%s)"), *What, Presses, *Key.ToString(), *Describe()));
				return true;
			}
			return true;
		}

	private:
		enum class EPhase : uint8 { Settle, Hold, Check };

		FKeyEvent Event() const
		{
			return FKeyEvent(Key, FModifierKeysState(), 0, false, 0, 0);
		}

		FAutomationTestBase* Test;
		FKey Key;
		FString What;
		TFunction<bool()> Done;
		TFunction<void()> Before;
		uint64 Hold;
		int32 MaxPresses;
		EPhase Phase = EPhase::Settle;
		int32 Presses = 0;
		uint64 SettledFrame = 0, PressedFrame = 0, ReleasedFrame = 0;
		double Started = 0, Released = 0;
	};

	/** Presses Key once; What must then be true. */
	void Press(FAutomationTestBase* Test, const FKey& Key, const FString& What, TFunction<bool()> Done = nullptr, TFunction<void()> Before = nullptr)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FPressKey(Test, Key, What, MoveTemp(Done), 3, 1, MoveTemp(Before)));
	}

	/** Holds Key down for Frames frames (zooming, panning, walking, firing). */
	void Hold(FAutomationTestBase* Test, const FKey& Key, uint64 Frames, const FString& What, TFunction<bool()> Done, TFunction<void()> Before = nullptr)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FPressKey(Test, Key, What, MoveTemp(Done), Frames, 1, MoveTemp(Before)));
	}

	/** Presses Key until the menu's selection is on Row (the rows wrap). */
	void PressToRow(FAutomationTestBase* Test, const FKey& Key, int32 Row, const FString& What)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FPressKey(Test, Key, What, [Row]() { return World()->Menu.GetItem() == Row; }, 3, 10, nullptr));
	}

	/** Runs Do once, after the keys before it. */
	void Then(FAutomationTestBase* Test, const FString& What, TFunction<void()> Do)
	{
		ADD_LATENT_AUTOMATION_COMMAND(DoomTest::FStep(Test, What, [Do]()
		{
			Do();
			return true;
		}));
	}

	/** E1M1 in PIE with the keyboard on the game. */
	void Start(FAutomationTestBase* Test)
	{
		DoomTest::StartGame(Test);
		DoomTest::LoadMap(Test, TEXT("E1M1"));
		ADD_LATENT_AUTOMATION_COMMAND(DoomTest::FStep(Test, TEXT("settle"), DoomTest::Wait(0.5f)));
		Then(Test, TEXT("focus the game"), [Test]()
		{
			// PIE gives the editor's play commands every key first (Esc is Stop); a packaged game has no
			// such hook, so this session's viewport drops it (it goes with the session)
			if (UGameViewportClient* VC = DoomTest::PlayWorld()->GetGameViewport())
			{
				VC->OnGameViewportInputKey().Unbind();
			}
			FocusGame(); // the real mouse is held off from StartGame to EndGame (DoomTestCommon.h)
			Test->TestFalse(TEXT("input is not locked"), World()->IsInputLocked());
			Test->TestEqual(TEXT("playing"), (int32)World()->GetState(), (int32)EDoomGameState::Playing);
		});
		// a new PIE session settles before the first key: a press in its first frames was lost now and
		// then (the automap test's Tab, which then left every later toggle one step off)
		ADD_LATENT_AUTOMATION_COMMAND(DoomTest::FStep(Test, TEXT("let the session settle"), [Until = MakeShared<uint64>(0), Since = MakeShared<double>(0)]()
		{
			if (*Until == 0)
			{
				*Until = GFrameCounter + 30;
				*Since = FPlatformTime::Seconds();
			}
			return GFrameCounter >= *Until && FPlatformTime::Seconds() - *Since >= 1.0;
		}));
		// and one key nothing is bound to: the session's first keyboard key switches CommonUI's input
		// type, which could swallow it (the prompt test's first Esc was lost now and then)
		Press(Test, EKeys::Insert, TEXT("warm up the keyboard"));
	}

	/** Ends PIE (and gives the mouse back). */
	void Finish(FAutomationTestBase* Test)
	{
		DoomTest::EndGame(Test);
	}

	bool InMenu(EMenuPage Page)
	{
		return World()->GetState() == EDoomGameState::Menu && World()->Menu.GetPage() == Page;
	}

	bool Playing()
	{
		return World()->GetState() == EDoomGameState::Playing && !ADoomPlayerController::IsMenuScreenShown(Controller());
	}

	/** The main menu's rows: Doom 1 has Read This!, Doom 2 doesn't. */
	int32 MainRow(const TCHAR* Name)
	{
		const TArray<FMenuRow> Rows = World()->Menu.Rows(*World());
		for (int32 i = 0; i < Rows.Num(); ++i)
		{
			if (Rows[i].Patch == Name) return i;
		}
		return INDEX_NONE;
	}
}

using namespace DoomKeyTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomInputMenuTest, "DoomUE.Input.MenuKeys", DoomTest::Flags)
bool FDoomInputMenuTest::RunTest(const FString& Parameters)
{
	// Esc opens the menu, the arrows move, Enter opens a page, Backspace goes back, Esc closes it all;
	// F1 opens Read This!, Enter turns its page and Esc closes it
	TSharedRef<int32> Row = MakeShared<int32>(0);
	Start(this);
	Press(this, EKeys::Escape, TEXT("Esc opens the main menu"), []() { return InMenu(EMenuPage::Main); });
	Press(this, EKeys::Down, TEXT("Down moves one row down"), [Row]() { return World()->Menu.GetItem() != *Row; },
		[Row]() { *Row = World()->Menu.GetItem(); });
	Press(this, EKeys::Up, TEXT("Up moves back"), [Row]() { return World()->Menu.GetItem() == *Row; });
	PressToRow(this, EKeys::Down, 1, TEXT("Down reaches Options"));
	Press(this, EKeys::Enter, TEXT("Enter opens Options"), []() { return InMenu(EMenuPage::Options); });
	// Pixels: Enter steps FULL, 640X400, 320X200 and round again, back to where it was
	TSharedRef<int32> Rows = MakeShared<int32>(0);
	auto PixelRows = []() { return GetDefault<UDoomSettings>()->ClassicPixelRows; };
	auto Next = [](int32 R) { return R == 0 ? 400 : R == 400 ? 200 : 0; };
	PressToRow(this, EKeys::Down, EOptionsRow::Pixels, TEXT("Down reaches Pixels"));
	for (int32 i = 1; i <= 3; ++i)
	{
		Press(this, EKeys::Enter, FString::Printf(TEXT("Enter steps Pixels (%d of 3)"), i),
			[Rows, PixelRows, Next, i]()
			{
				int32 Want = *Rows;
				for (int32 k = 0; k < i; ++k) Want = Next(Want);
				return PixelRows() == Want;
			},
			[Rows, PixelRows, i]() { if (i == 1) *Rows = PixelRows(); });
	}
	Press(this, EKeys::BackSpace, TEXT("Backspace goes back to the main menu, on Options"),
		[]() { return InMenu(EMenuPage::Main) && World()->Menu.GetItem() == 1; });
	Press(this, EKeys::Escape, TEXT("Esc closes the menu"), []() { return Playing(); });
	// Esc closes every page at once, as in Doom (not one page back)
	Press(this, EKeys::Escape, TEXT("Esc opens the menu again"), []() { return InMenu(EMenuPage::Main); });
	PressToRow(this, EKeys::Down, 1, TEXT("Down reaches Options"));
	Press(this, EKeys::Enter, TEXT("Enter opens Options"), []() { return InMenu(EMenuPage::Options); });
	Press(this, EKeys::Escape, TEXT("Esc on Options closes the whole menu"), []() { return Playing(); });
	Press(this, EKeys::F1, TEXT("F1 opens Read This!"), []() { return InMenu(EMenuPage::ReadThis); });
	Press(this, EKeys::Enter, TEXT("Enter turns the page"), []() { return InMenu(EMenuPage::ReadThis) && World()->Menu.GetHelpPage() == 1; });
	Press(this, EKeys::Escape, TEXT("Esc closes Read This!"), []() { return Playing(); });
	Finish(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomInputPromptTest, "DoomUE.Input.Prompts", DoomTest::Flags)
bool FDoomInputPromptTest::RunTest(const FString& Parameters)
{
	// the quit prompt: N cancels and the game goes on (Y would leave PIE, so it isn't pressed);
	// the end game prompt: a stray key cancels, Y ends the game
	TSharedRef<int32> Quit = MakeShared<int32>(INDEX_NONE);
	Start(this);
	Press(this, EKeys::Escape, TEXT("Esc opens the main menu"), []() { return InMenu(EMenuPage::Main); });
	Then(this, TEXT("find Quit Game"), [this, Quit]()
	{
		*Quit = MainRow(TEXT("M_QUITG"));
		TestTrue(TEXT("the main menu has Quit Game"), *Quit >= 0);
	});
	ADD_LATENT_AUTOMATION_COMMAND(FPressKey(this, EKeys::Down, TEXT("Down reaches Quit Game"), [Quit]() { return World()->Menu.GetItem() == *Quit; }, 3, 10, nullptr));
	Press(this, EKeys::Enter, TEXT("Enter on Quit Game asks"), []() { return InMenu(EMenuPage::Quit) && !World()->Menu.Prompt().IsEmpty(); });
	Press(this, EKeys::N, TEXT("N cancels the quit, the game goes on"), []() { return InMenu(EMenuPage::Main) && World()->GetMap(); });
	PressToRow(this, EKeys::Up, 1, TEXT("Up reaches Options"));
	Press(this, EKeys::Enter, TEXT("Enter opens Options"), []() { return InMenu(EMenuPage::Options); });
	PressToRow(this, EKeys::Up, EOptionsRow::EndGame, TEXT("Up reaches End Game"));
	Press(this, EKeys::Enter, TEXT("Enter on End Game asks"), []() { return InMenu(EMenuPage::EndGame); });
	Press(this, EKeys::K, TEXT("a stray key cancels: back on Options, still in the level"), []() { return InMenu(EMenuPage::Options) && World()->GetMap(); });
	PressToRow(this, EKeys::Up, EOptionsRow::EndGame, TEXT("Up reaches End Game again (the mouse may have moved the selection)"));
	Press(this, EKeys::Enter, TEXT("Enter on End Game asks again"), []() { return InMenu(EMenuPage::EndGame); });
	Press(this, EKeys::Y, TEXT("Y ends the game: the title screen"), []() { return World()->GetState() == EDoomGameState::Title; });
	Finish(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomInputAutomapTest, "DoomUE.Input.AutomapKeys", DoomTest::Flags)
bool FDoomInputAutomapTest::RunTest(const FString& Parameters)
{
	// Tab opens the map; = and - zoom while held, G grid, M mark, C clear, F follow off, then the
	// arrows pan; 0 the whole map and back; Tab closes it
	struct FView { float Scale = 0; FVector2f Center = FVector2f::ZeroVector; };
	TSharedRef<FView> V = MakeShared<FView>();
	auto View = []() -> const ADoomHUD::FAutomapView& { return Hud()->GetAutomapView(); };
	Start(this);
	Press(this, EKeys::Tab, TEXT("Tab opens the automap"), []() { return World()->IsAutomapOpen(); });
	Hold(this, EKeys::Equals, 30, TEXT("= held zooms in"), [V, View]() { return View().Scale > V->Scale * 1.1f; }, [V, View]() { V->Scale = View().Scale; });
	Hold(this, EKeys::Hyphen, 30, TEXT("- held zooms out"), [V, View]() { return View().Scale < V->Scale / 1.1f; }, [V, View]() { V->Scale = View().Scale; });
	Press(this, EKeys::G, TEXT("G shows the grid"), [View]() { return View().bGrid; });
	Press(this, EKeys::M, TEXT("M marks"), [View]() { return View().Marks.Num() == 1; });
	Press(this, EKeys::C, TEXT("C clears the marks"), [View]() { return View().Marks.Num() == 0; });
	Press(this, EKeys::F, TEXT("F stops following"), [View]() { return !View().bFollow; });
	Hold(this, EKeys::Right, 30, TEXT("Right held pans east"), [V, View]() { return View().Center.X > V->Center.X + 1.f; }, [V, View]() { V->Center = View().Center; });
	Press(this, EKeys::Zero, TEXT("0 shows the whole map"), [View]() { return View().bBig; });
	Press(this, EKeys::Zero, TEXT("0 again goes back"), [View]() { return !View().bBig; });
	Press(this, EKeys::F, TEXT("F follows again"), [View]() { return View().bFollow; });
	Press(this, EKeys::G, TEXT("G hides the grid"), [View]() { return !View().bGrid; });
	Press(this, EKeys::Tab, TEXT("Tab closes the automap"), []() { return !World()->IsAutomapOpen(); });
	Finish(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDoomInputPlayTest, "DoomUE.Input.Gameplay", DoomTest::Flags)
bool FDoomInputPlayTest::RunTest(const FString& Parameters)
{
	// W walks forward, Ctrl fires, 3 brings up the shotgun
	struct FState { FVector Start = FVector::ZeroVector; int32 Bullets = 0; };
	TSharedRef<FState> S = MakeShared<FState>();
	Start(this);
	Then(this, TEXT("face open space"), [S]()
	{
		// the clearest of 16 directions, so walking forward isn't stopped by a wall
		ADoomPlayer* P = World()->GetPlayer();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomKeyTest), false, P);
		float BestYaw = 0, Best = -1;
		for (int32 a = 0; a < 16; ++a)
		{
			const FVector From = P->GetActorLocation();
			FHitResult Hit;
			const float Dist = DoomTest::PlayWorld()->SweepSingleByChannel(Hit, From, From + FRotator(0, a * 22.5f, 0).Vector() * 512.f * Doom::Scale,
				FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(16.f * Doom::Scale), Q) ? Hit.Distance : 1e9f;
			if (Dist > Best) { Best = Dist; BestYaw = a * 22.5f; }
		}
		P->GetController()->SetControlRotation(FRotator(0, BestYaw, 0));
	});
	Hold(this, EKeys::W, 35, TEXT("W held walks more than 32 units"),
		[S]() { return FVector::Dist2D(World()->GetPlayer()->GetActorLocation(), S->Start) / Doom::Scale > 32.f; },
		[S]() { S->Start = World()->GetPlayer()->GetActorLocation(); });
	Hold(this, EKeys::LeftControl, 20, TEXT("Ctrl held fires"),
		[S]() { return World()->GetPlayer()->Inv().Ammo[(int32)EDoomAmmo::Bullets] < S->Bullets; },
		[S]() { S->Bullets = World()->GetPlayer()->Inv().Ammo[(int32)EDoomAmmo::Bullets]; });
	Press(this, EKeys::Three, TEXT("3 brings up the shotgun"),
		[]()
		{
			const ADoomPlayer* P = World()->GetPlayer();
			return P->GetPendingWeapon() == EDoomWeapon::Shotgun || P->Inv().Ready == EDoomWeapon::Shotgun;
		},
		[]()
		{
			ADoomPlayer* P = World()->GetPlayer();
			P->Inv().Weapons[(int32)EDoomWeapon::Shotgun] = true;
			P->Inv().Ammo[(int32)EDoomAmmo::Shells] = 8;
		});
	Finish(this);
	return true;
}

#endif

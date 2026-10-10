#include "DoomGameMode.h"
#include "DoomWorld.h"
#include "DoomPlayer.h"
#include "DoomPlayerController.h"
#include "DoomHUD.h"
#include "DoomSettings.h"
#include "DoomWad.h"
#include "DoomUE.h"
#include "DoomMusic.h"
#include "DoomMonster.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"
#include "TimerManager.h"
#include "Misc/Paths.h"

ADoomGameMode::ADoomGameMode()
{
	DefaultPawnClass = ADoomPlayer::StaticClass();
	PlayerControllerClass = ADoomPlayerController::StaticClass();
	HUDClass = ADoomHUD::StaticClass();
}

void ADoomGameMode::RestartPlayer(AController* NewPlayer)
{
	if (NewPlayer && !NewPlayer->IsPendingKillPending())
	{
		RestartPlayerAtTransform(NewPlayer, FTransform(FRotator::ZeroRotator, FVector(0, 0, 100000.f)));
		if (APawn* Pawn = NewPlayer->GetPawn())
		{
			// parked until a map is loaded
			if (ACharacter* C = Cast<ACharacter>(Pawn))
			{
				C->GetCharacterMovement()->SetMovementMode(MOVE_None);
			}
		}
	}
}

void ADoomGameMode::StartPlay()
{
	Super::StartPlay();
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (!W || !W->InitWads())
	{
		UE_LOG(LogDoom, Error, TEXT("No usable IWAD. Put one in Content/WADs or pass -iwad=<path>"));
		return;
	}
	int32 LoadSlot = -1;
	if (FParse::Value(FCommandLine::Get(), TEXT("-loadgame="), LoadSlot) && W->LoadGame(LoadSlot))
	{
		return;
	}
	FString Warp = GetDefault<UDoomSettings>()->StartMap;
	FParse::Value(FCommandLine::Get(), TEXT("-warp="), Warp);
	if (!Warp.IsEmpty())
	{
		// -warp=1 1 style (episode map) or -warp=E1M1 / MAP01
		if (Warp.IsNumeric() && W->GetWad()->IsDoom2())
		{
			Warp = FString::Printf(TEXT("MAP%02d"), FCString::Atoi(*Warp));
		}
		if (!W->LoadMap(Warp, true))
		{
			W->SetState(EDoomGameState::Title);
		}
	}
	else
	{
		W->SetState(EDoomGameState::Title);
		W->PlayTitleMusic();
	}

	// -maptest=a.wad,b.wad[:MAP]: load a map from each PWAD over the IWAD, log its node format and area
	FString MapTest;
	if (FParse::Value(FCommandLine::Get(), TEXT("-maptest="), MapTest, false))
	{
		TArray<FString> Files;
		MapTest.ParseIntoArray(Files, TEXT(","));
		for (const FString& Entry : Files)
		{
			FString File = Entry, MapName = TEXT("E1M1");
			Entry.Split(TEXT(":"), &File, &MapName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (File.Len() <= 2) { File = Entry; MapName = TEXT("E1M1"); } // drive letter, not a map
			FDoomWad TestWad;
			TestWad.AddFile(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("WADs"), W->GetWad()->IsDoom2() ? TEXT("freedoom2.wad") : TEXT("freedoom1.wad")));
			TestWad.AddFile(File);
			TestWad.Finalize();
			FDoomMap TestMap;
			const bool bOk = TestMap.Load(TestWad, MapName);
			double Area = 0;
			for (const FDoomSubsector& Ss : TestMap.Subsectors)
			{
				for (int32 k = 1; k + 1 < Ss.Polygon.Num(); ++k)
				{
					const FVector2f A = Ss.Polygon[k] - Ss.Polygon[0], B = Ss.Polygon[k + 1] - Ss.Polygon[0];
					Area += 0.5 * (A.X * B.Y - A.Y * B.X);
				}
			}
			UE_LOG(LogDoom, Display, TEXT("Maptest %s %s: %s, %s nodes, %d subsectors, area %.0f"), *FPaths::GetCleanFilename(File), *MapName,
				bOk ? TEXT("ok") : TEXT("FAILED"), *TestMap.NodesFormat, TestMap.Subsectors.Num(), Area);
		}
	}

	// -patchtest: log what DeHackEd / UMAPINFO / Boom lumps changed
	if (FParse::Param(FCommandLine::Get(), TEXT("patchtest")))
	{
		const FDoomThingDef* Imp = DoomInfo::FindThing(3001);
		const FDoomProjectileDef* Ball = DoomInfo::FindProjectile(TEXT("ImpBall"));
		const FDoomRules& R = DoomInfo::Rules();
		const FDoomMapInfo* Info = W->MapInfo(TEXT("E1M1"));
		UE_LOG(LogDoom, Display, TEXT("Patchtest imp: health %d radius %.0f | rules: health %d bullets %d maxbullets %d clip %d | impball speed %.0f damage %d"),
			Imp->Health, Imp->Radius, R.InitialHealth, R.InitialBullets, R.MaxAmmo[0], R.ClipAmmo[0], Ball->Speed, Ball->DamageMul);
		UE_LOG(LogDoom, Display, TEXT("Patchtest E1M1: title \"%s\" par %d music %s sky %s next %s | E1M3 title \"%s\" | GOTCLIP \"%s\""),
			*W->LevelTitle(TEXT("E1M1")), DoomInfo::ParTime(TEXT("E1M1")), Info ? *Info->Music : TEXT("-"), Info ? *Info->Sky : TEXT("-"),
			Info ? *Info->Next : TEXT("-"), *W->LevelTitle(TEXT("E1M3")), *DoomInfo::PickupMessage(EPickup::Clip));
		UE_LOG(LogDoom, Display, TEXT("Patchtest switches: SW1BRN1 -> %s, SW1GRAY -> %s, SW1HOT -> %s | NUKAGE1 animated %d, FWATER1 animated %d"),
			*W->GetGraphics()->SwitchPartner(TEXT("SW1BRN1")).ToString(), *W->GetGraphics()->SwitchPartner(TEXT("SW1GRAY")).ToString(),
			*W->GetGraphics()->SwitchPartner(TEXT("SW1HOT")).ToString(), W->GetGraphics()->IsAnimated(TEXT("NUKAGE1"), true), W->GetGraphics()->IsAnimated(TEXT("FWATER1"), true));
	}

	// -dumpmusic=D_E1M1[,seconds]: render a song to Saved/music_<lump>.wav and log its statistics
	FString Dump;
	if (FParse::Value(FCommandLine::Get(), TEXT("-dumpmusic="), Dump, false))
	{
		FString Lump = Dump, Secs = TEXT("30");
		Dump.Split(TEXT(","), &Lump, &Secs);
		FString Stats;
		const FString Path = FPaths::ProjectSavedDir() / FString::Printf(TEXT("music_%s.wav"), *Lump.ToUpper());
		const bool bOk = DoomMusic::RenderToWav(*W->GetWad(), Lump.ToUpper(), FCString::Atof(*Secs), Path, Stats);
		UE_LOG(LogDoom, Display, TEXT("Dumpmusic %s: %s -> %s"), bOk ? TEXT("ok") : TEXT("FAILED"), *Stats, *Path);
	}

	// -endtest [-endpress=N]: finish the warped map straight into its ending; press Enter N times
	if (FParse::Param(FCommandLine::Get(), TEXT("endtest")) && W->GetMap())
	{
		int32 Presses = 0;
		FParse::Value(FCommandLine::Get(), TEXT("-endpress="), Presses);
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [W, Presses]()
		{
			W->ExitLevel(false);
			W->FinishIntermission();
			for (int32 i = 0; i < Presses; ++i)
			{
				W->FinaleTic = 100000;
				W->FinaleInput();
			}
			if (W->GetState() == EDoomGameState::Finale && W->FinaleStage == UDoomWorld::EFinaleStage::Text)
			{
				W->FinaleTic = 100000; // whole text for the screenshot
			}
			UE_LOG(LogDoom, Display, TEXT("Endtest: state %d stage %d picture %s text lines %d cast %s"),
				(int32)W->GetState(), (int32)W->FinaleStage, *W->FinalePicture, W->FinaleText.Num(), *W->CastName());
		}, 1.f, false);
	}

	// -menupage=N [-menuitem=N]: open a menu page (EMenuPage order) for screenshots
	int32 MenuPage = -1;
	if (FParse::Value(FCommandLine::Get(), TEXT("-menupage="), MenuPage))
	{
		int32 MenuItem = 0;
		FParse::Value(FCommandLine::Get(), TEXT("-menuitem="), MenuItem);
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [W, MenuPage, MenuItem]()
		{
			W->Menu.Open(*W, (EMenuPage)MenuPage);
			W->Menu.Item = MenuItem;
		}, 1.f, false);
	}

	// -spawnfront=DoomEdNum: a monster 160 units in front of the player (spectre fuzz checks)
	int32 SpawnFront = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("-spawnfront="), SpawnFront) && W->GetMap())
	{
		if (ADoomPlayer* P = W->GetPlayer())
		{
			const FVector Fwd = FRotator(0, P->GetControlRotation().Yaw, 0).Vector();
			const FVector Feet = P->GetActorLocation() - FVector(0, 0, P->GetSimpleCollisionHalfHeight()) + Fwd * 160.f * Doom::Scale;
			W->SpawnMonster(SpawnFront, Feet, Doom::YawToAngle(P->GetControlRotation().Yaw) + 180.f);
		}
	}

	// -steptest=x,y,angle,seconds[,sector]: place the player at Doom (x, y) facing angle, walk forward,
	// log position / height (and that sector's floor) every 0.25 s; for movement and step checks
	FString StepTest;
	if (FParse::Value(FCommandLine::Get(), TEXT("-steptest="), StepTest, false) && W->GetMap())
	{
		TArray<FString> A;
		StepTest.ParseIntoArray(A, TEXT(","));
		if (A.Num() >= 4)
		{
			const FVector2f Start(FCString::Atof(*A[0]), FCString::Atof(*A[1]));
			const float Angle = FCString::Atof(*A[2]);
			const float Seconds = FCString::Atof(*A[3]);
			const int32 WatchSector = A.Num() >= 5 ? FCString::Atoi(*A[4]) : -1;
			FTimerHandle T;
			GetWorldTimerManager().SetTimer(T, [this, W, Start, Angle, Seconds, WatchSector]()
			{
				ADoomPlayer* P = W->GetPlayer();
				if (!P) return;
				const int32 Sec = W->GetMap()->SectorAt(Start);
				const float Floor = Sec >= 0 ? W->GetMap()->Sectors[Sec].Floor : 0;
				P->SetActorLocation(Doom::ToUE(Start, Floor) + FVector(0, 0, P->GetSimpleCollisionHalfHeight() + 1), false, nullptr, ETeleportType::TeleportPhysics);
				P->GetController()->SetControlRotation(FRotator(0, Doom::AngleToYaw(Angle), 0));
				const double End = GetWorld()->GetTimeSeconds() + Seconds;
				double NextLog = 0;
				FTimerHandle Walk;
				GetWorldTimerManager().SetTimer(Walk, [this, W, P, End, NextLog, WatchSector]() mutable
				{
					const double Now = GetWorld()->GetTimeSeconds();
					if (Now > End) return;
					P->AddMovementInput(FRotator(0, P->GetControlRotation().Yaw, 0).Vector(), 1.f);
					if (Now >= NextLog)
					{
						NextLog = Now + 0.25;
						const FVector2f D = P->DoomPos();
						UE_LOG(LogDoom, Display, TEXT("Steptest t=%.2f doom(%.0f,%.0f) z=%.1f sector %d vel %.0f%s"), Now, D.X, D.Y, P->DoomZ(),
							W->GetMap()->SectorAt(D), P->GetVelocity().Size2D() / Doom::Scale,
							WatchSector >= 0 ? *FString::Printf(TEXT(" | sector %d floor %.0f"), WatchSector, W->GetMap()->Sectors[WatchSector].Floor) : TEXT(""));
					}
				}, 0.01f, true);
			}, 1.f, false);
		}
	}

	// -killtype=3001: after 2 s kill every monster of that DoomEdNum (boss-action checks)
	int32 KillType = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("-killtype="), KillType) && W->GetMap())
	{
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [W, KillType]()
		{
			int32 Killed = 0;
			for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
			{
				if (M.IsValid() && M->Def->DoomEdNum == KillType && !M->IsDead())
				{
					UGameplayStatics::ApplyDamage(M.Get(), 100000.f, nullptr, W->GetPlayer(), nullptr);
					++Killed;
				}
			}
			UE_LOG(LogDoom, Display, TEXT("Killtype %d: killed %d, active movers %d"), KillType, Killed, W->NumMovers());
		}, 2.f, false);
	}

	// -savetest: save mid-action, scramble, load, compare (needs -warp)
	if (FParse::Param(FCommandLine::Get(), TEXT("savetest")) && W->GetMap())
	{
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [W]() { W->RunSaveTest(); }, 2.f, false);
	}

	// -autotest[=seconds] [-autoyaw=deg]: screenshot Saved/Screenshots/autotest_<map>.png and quit
	FString AutoTest;
	if (FParse::Value(FCommandLine::Get(), TEXT("-autotest="), AutoTest) || FParse::Param(FCommandLine::Get(), TEXT("autotest")))
	{
		const float Delay = AutoTest.IsEmpty() ? 4.f : FCString::Atof(*AutoTest);
		// deterministic captures: stray clicks or keys on the test window must not play the game
		W->bInputLocked = true;
		float Yaw = 0, Pitch = 0;
		const bool bYaw = FParse::Value(FCommandLine::Get(), TEXT("-autoyaw="), Yaw);
		const bool bPitch = FParse::Value(FCommandLine::Get(), TEXT("-autopitch="), Pitch);
		FTimerHandle Shot, Quit;
		GetWorldTimerManager().SetTimer(Shot, [this, W, Yaw, bYaw, Pitch, bPitch]()
		{
			if (bYaw || bPitch)
			{
				if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
				{
					PC->SetControlRotation(FRotator(Pitch, PC->GetControlRotation().Yaw + Yaw, 0));
				}
			}
			GetWorldTimerManager().SetTimerForNextTick([W]()
			{
				const FString Name = FString::Printf(TEXT("autotest_%s.png"), W->GetMapName().IsEmpty() ? TEXT("title") : *W->GetMapName());
				FScreenshotRequest::RequestScreenshot(FPaths::ScreenShotDir() / Name, true, false);
				UE_LOG(LogDoom, Display, TEXT("Autotest: screenshot %s, kills %d/%d items %d/%d | %s"), *Name, W->Kills, W->TotalKills, W->Items, W->TotalItems, *W->StateFingerprint());
			});
		}, Delay, false);
		GetWorldTimerManager().SetTimer(Quit, []() { FPlatformMisc::RequestExit(false, TEXT("DoomAutotest")); }, Delay + 2.f, false);
	}
}

void ADoomGameMode::DoomMap(const FString& MapName)
{
	if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>())
	{
		W->LoadMap(MapName, false);
	}
}

void ADoomGameMode::DoomGod()
{
	if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>())
	{
		if (ADoomPlayer* P = W->GetPlayer())
		{
			P->bGodMode = !P->bGodMode;
			W->ShowMessage(P->bGodMode ? TEXT("God mode on") : TEXT("God mode off"));
		}
	}
}

void ADoomGameMode::DoomGiveAll()
{
	if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>())
	{
		if (ADoomPlayer* P = W->GetPlayer())
		{
			P->Inv.bBackpack = true;
			for (bool& B : P->Inv.Weapons) B = true;
			for (int32 i = 0; i < 4; ++i) P->Inv.Ammo[i] = DoomInfo::MaxAmmo((EDoomAmmo)i, true);
			for (bool& K : P->Inv.Keys) K = true;
			P->Inv.Armor = 200;
			P->Inv.ArmorClass = 2;
			W->ShowMessage(TEXT("All weapons, ammo and keys"));
		}
	}
}

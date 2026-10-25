#include "DoomGameMode.h"
#include "DoomWorld.h"
#include "DoomEditor.h"
#include "DoomPlayer.h"
#include "DoomPlayerController.h"
#include "DoomHUD.h"
#include "DoomSettings.h"
#include "DoomWad.h"
#include "DoomUE.h"
#include "DoomMusic.h"
#include "DoomMonster.h"
#include "DoomLevel.h"
#include "DoomMapActors.h"
#include "DoomGameState.h"
#include "DoomPlayerState.h"
#include "GameFramework/GameSession.h"
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
	PlayerStateClass = ADoomPlayerState::StaticClass();
	GameStateClass = ADoomGameState::StaticClass();
	HUDClass = ADoomHUD::StaticClass();
	// the clock ticks after the frame's movement and physics, as the world subsystem's tick did
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

UDoomWorld* ADoomGameMode::DoomWorld() const
{
	return GetWorld() ? GetWorld()->GetSubsystem<UDoomWorld>() : nullptr;
}

void ADoomGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UDoomWorld* W = DoomWorld();
	if (!W || !W->GetWad())
	{
		return;
	}
	if (W->IsLoadingMap())
	{
		// the loading screen keeps drawing: a few milliseconds of the load per frame
		W->StepLoad(FPlatformTime::Seconds() + 0.008);
		Accumulator = ScreenAccumulator = 0;
		return;
	}
	FDoomLevelFlow& Flow = W->Flow();
	const double Delta = FMath::Min(DeltaSeconds, 0.25f); // a long hitch is not worth a burst of tics
	if (Flow.Phase == EDoomGameState::Intermission || Flow.Phase == EDoomGameState::Finale)
	{
		// screens between levels count real 35 Hz tics, whatever the frame rate
		ScreenAccumulator += Delta;
		while (ScreenAccumulator >= Doom::TicSeconds)
		{
			ScreenAccumulator -= Doom::TicSeconds;
			if (Flow.Phase == EDoomGameState::Intermission)
			{
				++Flow.IntermissionTic;
			}
			else
			{
				W->TicFinale();
				if (W->IsNetServer())
				{
					if (ADoomGameState* GS = GetGameState<ADoomGameState>()) GS->PublishFinale(*W);
				}
			}
		}
	}
	if (Flow.Phase != EDoomGameState::Playing || !W->GetMap())
	{
		return;
	}
	// a stopped clock (screenshot tests) runs no tics but still publishes the material globals
	Accumulator = bClockStopped ? 0 : Accumulator + Delta;
	while (Accumulator >= Doom::TicSeconds)
	{
		Accumulator -= Doom::TicSeconds;
		W->RunTic();
		if (W->Flow().Phase != EDoomGameState::Playing || !W->GetMap())
		{
			return; // the tic left the level (exit, death screen, menu load)
		}
	}
	if (W->IsNetServer())
	{
		// clients see moved planes, light changes and switched textures
		if (ADoomGameState* GS = GetGameState<ADoomGameState>())
		{
			GS->PublishWorld(*W->GetMap(), W->BaselineSectors, W->BaselineSides);
		}
	}
	W->UpdateMaterialGlobals(Accumulator / Doom::TicSeconds);
}

namespace DoomPerfTest { void Start(UWorld* World); }

void ADoomGameMode::DoomEdit()
{
	if (UDoomMapEditor* E = GetWorld()->GetSubsystem<UDoomMapEditor>())
	{
		E->Toggle();
	}
}

FString ADoomGameMode::NextMapName(bool bSecret) const
{
	const UDoomWorld* W = DoomWorld();
	if (!W)
	{
		return FString();
	}
	const FString& M = W->Flow().CurrentMap;
	uint32 GenSeed = 0;
	if (DoomMapGen::ParseName(M, GenSeed))
	{
		return DoomMapGen::NameForSeed(GenSeed + 1); // generated maps go on for ever
	}
	if (const FDoomMapInfo* Info = W->MapInfo(M))
	{
		if (Info->bEndGame)
		{
			return FString();
		}
		if (bSecret && !Info->NextSecret.IsEmpty())
		{
			return Info->NextSecret;
		}
		if (!Info->Next.IsEmpty())
		{
			return Info->Next;
		}
	}
	if (M.StartsWith(TEXT("MAP")))
	{
		const int32 N = FCString::Atoi(*M.Mid(3));
		int32 Next = N + 1;
		if (bSecret && N == 15) Next = 31;
		else if (bSecret && N == 31) Next = 32;
		else if (N == 31 || N == 32) Next = 16;
		else if (N == 30) Next = 0;
		return Next > 0 ? FString::Printf(TEXT("MAP%02d"), Next) : FString();
	}
	if (M.Len() == 4)
	{
		const int32 E = M[1] - '0';
		const int32 N = M[3] - '0';
		static const int32 SecretFrom[5] = { 0, 3, 5, 6, 2 };
		if (bSecret) return FString::Printf(TEXT("E%dM9"), E);
		if (N == 9) return FString::Printf(TEXT("E%dM%d"), E, SecretFrom[FMath::Clamp(E, 1, 4)] + 1);
		if (N == 8) return FString(); // end of episode
		return FString::Printf(TEXT("E%dM%d"), E, N + 1);
	}
	return FString();
}

void ADoomGameMode::ExitLevel(bool bSecret)
{
	UDoomWorld* W = DoomWorld();
	if (!W || W->Flow().Phase != EDoomGameState::Playing)
	{
		return;
	}
	FDoomLevelFlow& Flow = W->Flow();
	Flow.bSecretExit = bSecret;
	Flow.PendingMap = NextMapName(bSecret);
	uint32 GenSeed = 0;
	if (!Flow.PendingMap.IsEmpty() && !DoomMapGen::ParseName(Flow.PendingMap, GenSeed) && W->GetWad()->FindLump(Flow.PendingMap) == INDEX_NONE)
	{
		Flow.PendingMap.Reset();
	}
	Flow.IntermissionTic = 0;
	ScreenAccumulator = 0;
	if (ADoomPlayer* P = W->GetPlayer())
	{
		P->GetCharacterMovement()->StopMovementImmediately();
	}
	const FDoomMapInfo* Info = W->MapInfo(Flow.CurrentMap);
	Flow.Phase = EDoomGameState::Intermission; // FinishIntermission expects to leave it
	if (Info && Info->bNoIntermission)
	{
		FinishIntermission();
		return;
	}
	W->PlayMusic((Info && !Info->InterMusic.IsEmpty()) ? Info->InterMusic : W->GetWad()->IsDoom2() ? TEXT("D_DM2INT") : TEXT("D_INTER"));
}

void ADoomGameMode::FinishIntermission()
{
	UDoomWorld* W = DoomWorld();
	if (!W)
	{
		return;
	}
	const FDoomLevelFlow& Flow = W->Flow();
	// UMAPINFO story text between maps, else Doom's own episode / chapter texts
	const FDoomMapInfo* Info = W->MapInfo(Flow.CurrentMap);
	if (Info && !Info->bClearInterText)
	{
		const TArray<FString>& Text = (Flow.bSecretExit && Info->InterTextSecret.Num()) ? Info->InterTextSecret : Info->InterText;
		if (Text.Num())
		{
			W->StartFinaleText(Text, Info->InterBackdrop.IsEmpty() ? FString(W->GetWad()->IsDoom2() ? TEXT("SLIME16") : TEXT("FLOOR4_8")) : Info->InterBackdrop);
			return;
		}
	}
	if (!(Info && Info->bClearInterText) && W->StartVanillaText())
	{
		return;
	}
	if (Flow.PendingMap.IsEmpty())
	{
		W->StartEnding();
		return;
	}
	W->LoadMapAsync(Flow.PendingMap, false);
}

void ADoomGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	OptMap = UGameplayStatics::ParseOption(Options, TEXT("DoomMap"));
	OptRules = UGameplayStatics::HasOption(Options, TEXT("DoomRules")) ? FCString::Atoi(*UGameplayStatics::ParseOption(Options, TEXT("DoomRules"))) : -1;
	bOptNoMonsters = UGameplayStatics::HasOption(Options, TEXT("NoMonsters"));
	bOptFast = UGameplayStatics::HasOption(Options, TEXT("Fast"));
	if (GameSession)
	{
		GameSession->MaxPlayers = 4; // Doom's MAXPLAYERS
	}
}

void ADoomGameMode::PostLogin(APlayerController* NewPlayer)
{
	if (ADoomPlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<ADoomPlayerState>() : nullptr)
	{
		TSet<int32> Taken;
		if (const ADoomGameState* GS = GetGameState<ADoomGameState>())
		{
			for (const ADoomPlayerState* Other : GS->DoomPlayers())
			{
				if (Other != PS) Taken.Add(Other->PlayerNumber);
			}
		}
		int32 Number = 0;
		while (Taken.Contains(Number)) ++Number;
		PS->PlayerNumber = FMath::Min(Number, 3);
	}
	Super::PostLogin(NewPlayer);
}

void ADoomGameMode::RespawnPlayer(AController* Controller)
{
	UDoomWorld* W = DoomWorld();
	if (!Controller || !W)
	{
		return;
	}
	if (APawn* Old = Controller->GetPawn())
	{
		Controller->UnPossess();
		Old->Destroy();
	}
	if (ADoomPlayerState* PS = Controller->GetPlayerState<ADoomPlayerState>())
	{
		PS->Inv.Reset();
		PS->Inv.Health = DoomInfo::Rules().InitialHealth;
		PS->Inv.Ammo[0] = DoomInfo::Rules().InitialBullets;
	}
	RestartPlayer(Controller);
	if (const APawn* P = Controller->GetPawn())
	{
		W->SpawnEffect(TEXT("TFOG"), TEXT("ABCDEFGHIJ"), 6, P->GetActorLocation() - FVector(0, 0, Doom::HalfHeight(P)), true);
		const FVector At = P->GetActorLocation();
		W->PlaySound(TEXT("TELEPT"), &At);
	}
}

void ADoomGameMode::RestartPlayer(AController* NewPlayer)
{
	UDoomWorld* W = DoomWorld();
	if (NewPlayer && !NewPlayer->IsPendingKillPending() && W && W->GetMap() && W->Flow().Phase != EDoomGameState::Title)
	{
		// joining (or reborn) during a level: straight to this player's start
		RestartPlayerAtTransform(NewPlayer, FTransform(FRotator::ZeroRotator, FVector(0, 0, 100000.f)));
		if (ADoomPlayer* P = Cast<ADoomPlayer>(NewPlayer->GetPawn()))
		{
			P->InitForLevel(W, P->Inv().Health <= 0);
			W->PlacePlayer(P);
		}
		return;
	}
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
		UE_LOG(LogDoomGame, Error, TEXT("No usable IWAD. Put one in Content/WADs or pass -iwad=<path>"));
		return;
	}
	int32 LoadSlot = -1;
	if (FParse::Value(FCommandLine::Get(), TEXT("-loadgame="), LoadSlot) && W->LoadGame(LoadSlot))
	{
		return;
	}
	FString Warp = GetDefault<UDoomSettings>()->StartMap;
	// playing an imported map level starts its map (unless -warp says otherwise)
	if (const ADoomMapActor* MapActor = ADoomMapActor::Find(GetWorld()))
	{
		Warp = MapActor->MapName;
	}
	FParse::Value(FCommandLine::Get(), TEXT("-warp="), Warp);
	// a game hosted from the menu: its map and rules
	if (!OptMap.IsEmpty())
	{
		Warp = OptMap;
	}
	FDoomGameOptions Options = W->GetOptions();
	if (OptRules >= 0)
	{
		Options.bDeathmatch = OptRules >= 1;
		Options.bAltDeath = OptRules == 2;
	}
	Options.bNoMonsters |= bOptNoMonsters;
	Options.bFast |= bOptFast;
	W->SetOptions(Options);
	if (!OptMap.IsEmpty() || OptRules >= 0)
	{
		UE_LOG(LogDoomNet, Log, TEXT("Hosted game: %s, %s%s%s"), *Warp, OptRules == 2 ? TEXT("deathmatch 2.0") : OptRules == 1 ? TEXT("deathmatch") : TEXT("co-op"),
			Options.bNoMonsters ? TEXT(", no monsters") : TEXT(""), Options.bFast ? TEXT(", fast") : TEXT(""));
	}
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
			const double Area = TestMap.SubsectorArea();
			UE_LOG(LogDoomTest, Display, TEXT("Maptest %s %s: %s, %s nodes, %d subsectors, area %.0f"), *FPaths::GetCleanFilename(File), *MapName,
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
		UE_LOG(LogDoomTest, Display, TEXT("Patchtest imp: health %d radius %.0f | rules: health %d bullets %d maxbullets %d clip %d | impball speed %.0f damage %d"),
			Imp->Health, Imp->Radius, R.InitialHealth, R.InitialBullets, R.MaxAmmo[0], R.ClipAmmo[0], Ball->Speed, Ball->DamageMul);
		UE_LOG(LogDoomTest, Display, TEXT("Patchtest E1M1: title \"%s\" par %d music %s sky %s next %s | E1M3 title \"%s\" | GOTCLIP \"%s\""),
			*W->LevelTitle(TEXT("E1M1")), DoomInfo::ParTime(TEXT("E1M1")), Info ? *Info->Music : TEXT("-"), Info ? *Info->Sky : TEXT("-"),
			Info ? *Info->Next : TEXT("-"), *W->LevelTitle(TEXT("E1M3")), *DoomInfo::PickupMessage(EPickup::Clip));
		UE_LOG(LogDoomTest, Display, TEXT("Patchtest renames: TROO -> %s, BON1 -> %s, sound PISTOL -> %s"),
			*DoomInfo::SpriteName(TEXT("TROO")), *DoomInfo::SpriteName(TEXT("BON1")), *DoomInfo::SoundName(TEXT("PISTOL")));
		UE_LOG(LogDoomTest, Display, TEXT("Patchtest switches: SW1BRN1 -> %s, SW1GRAY -> %s, SW1HOT -> %s | NUKAGE1 animated %d, FWATER1 animated %d"),
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
		UE_LOG(LogDoomTest, Display, TEXT("Dumpmusic %s: %s -> %s"), bOk ? TEXT("ok") : TEXT("FAILED"), *Stats, *Path);
	}

	// -nettest (listen server): wait for a client, open a door, kill two monsters as the host (in
	// deathmatch also the client: a frag), exit to the intermission, quit; logs NetFingerprint
	if (FParse::Param(FCommandLine::Get(), TEXT("nettest")) && W->IsNetServer())
	{
		W->SetInputLocked(true); // stray keys on the test window must not play the host
		TSharedRef<float> Clock = MakeShared<float>(0.f);
		TSharedRef<float> Waited = MakeShared<float>(0.f);
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [this, W, Clock, Waited]()
		{
			const TArray<ADoomPlayer*> Players = W->GetPlayers();
			if (Players.Num() < 2)
			{
				*Waited += 0.5f;
				if (*Waited > 1800.f)
				{
					UE_LOG(LogDoomTest, Error, TEXT("Nettest server: no client joined"));
					FPlatformMisc::RequestExit(false, TEXT("DoomNettest"));
				}
				return;
			}
			*Clock += 0.5f;
			const float Now = *Clock;
			ADoomPlayer* Host = W->GetPlayer();
			ADoomPlayer* Guest = nullptr;
			for (ADoomPlayer* P : Players) if (P != Host) Guest = P;
			if (FMath::IsNearlyEqual(Now, 1.f))
			{
				UE_LOG(LogDoomTest, Display, TEXT("Nettest server: host at (%.0f,%.0f) guest at (%.0f,%.0f), deathmatch %d"),
					Host->DoomPos().X, Host->DoomPos().Y, Guest->DoomPos().X, Guest->DoomPos().Y, (int32)W->IsDeathmatch());
			}
			if (FMath::IsNearlyEqual(Now, 2.f))
			{
				W->NetTestOpenDoor();
				int32 Killed = 0;
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
				{
					if (Killed < 2 && M.IsValid() && !M->IsDead())
					{
						UGameplayStatics::ApplyDamage(M.Get(), 10000.f, Host->GetController(), Host, nullptr);
						++Killed;
					}
				}
				if (W->IsDeathmatch() && Guest)
				{
					UGameplayStatics::ApplyDamage(Guest, 10000.f, Host->GetController(), Host, nullptr);
				}
			}
			if (FMath::IsNearlyEqual(Now, 5.5f) && Guest)
			{
				// look at the guest for the screenshot
				const FRotator Look = (Guest->GetActorLocation() - Host->GetActorLocation()).Rotation();
				Host->GetController()->SetControlRotation(FRotator(0, Look.Yaw, 0));
			}
			if (FMath::IsNearlyEqual(Now, 6.f))
			{
				FScreenshotRequest::RequestScreenshot(FPaths::ScreenShotDir() / TEXT("net_server_play.png"), true, false);
			}
			if (FMath::Fmod(Now, 1.f) < 0.01f)
			{
				UE_LOG(LogDoomTest, Display, TEXT("Nettest server t=%.0f: %s"), Now, *W->NetFingerprint());
			}
			if (FMath::IsNearlyEqual(Now, 14.f))
			{
				ExitLevel(false);
			}
			if (FMath::IsNearlyEqual(Now, 45.f))
			{
				FPlatformMisc::RequestExit(false, TEXT("DoomNettest"));
			}
		}, 0.5f, true);
	}

	// -exittest: kill two monsters as the player, take the normal exit, wait in the intermission, go on;
	// checks the game state's totals, the player state's credit and the inventory carried to the next map
	if (FParse::Param(FCommandLine::Get(), TEXT("exittest")) && W->GetMap())
	{
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [this, W]()
		{
			ADoomPlayer* P = W->GetPlayer();
			ADoomPlayerState* PS = P ? P->GetDoomPlayerState() : nullptr;
			if (!P || !PS)
			{
				UE_LOG(LogDoomTest, Error, TEXT("Exittest FAIL: no player or no ADoomPlayerState"));
				return;
			}
			int32 Killed = 0;
			for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
			{
				if (Killed < 2 && M.IsValid() && !M->IsDead())
				{
					UGameplayStatics::ApplyDamage(M.Get(), 10000.f, P->GetController(), P, nullptr);
					++Killed;
				}
			}
			P->Inv().Ammo[1] = 17; // something to carry over
			const FString From = W->GetMapName();
			const int32 Kills = W->Flow().Kills, Credit = PS->LevelKills;
			ExitLevel(false);
			const bool bIntermission = W->GetState() == EDoomGameState::Intermission;
			const FString Pending = W->Flow().PendingMap;
			FTimerHandle T2;
			GetWorldTimerManager().SetTimer(T2, [this, W, PS, From, Kills, Credit, bIntermission, Pending]()
			{
				const int32 InterTics = W->Flow().IntermissionTic;
				FinishIntermission();
				// the next map loads in the background, behind the loading screen: check once it plays
				const bool bBackground = W->IsLoadingMap();
				const double LoadStart = FPlatformTime::Seconds();
				TSharedRef<FTimerHandle> T3 = MakeShared<FTimerHandle>();
				GetWorldTimerManager().SetTimer(*T3, [this, W, PS, From, Kills, Credit, bIntermission, Pending, InterTics, bBackground, LoadStart, T3]()
				{
					if (W->IsLoadingMap())
					{
						// the loading screen, once (Saved/Screenshots/.../exittest_loading.png)
						static bool bShot = false;
						if (!bShot)
						{
							bShot = true;
							FScreenshotRequest::RequestScreenshot(FPaths::ScreenShotDir() / TEXT("exittest_loading.png"), true, false);
						}
						return;
					}
					const bool bPass = Kills == 2 && Credit == 2 && bIntermission && InterTics > 30 && bBackground && W->GetMapName() == Pending
						&& W->GetState() == EDoomGameState::Playing && W->Flow().Kills == 0 && PS->LevelKills == 0 && PS->Inv.Ammo[1] == 17;
					UE_LOG(LogDoomTest, Display, TEXT("Exittest %s: %s kills %d (player credit %d), intermission %d (%d tics), next %s -> now %s (loaded in the background: %d, %.0f ms), kills %d / credit %d, shells %d"),
						bPass ? TEXT("PASS") : TEXT("FAIL"), *From, Kills, Credit, bIntermission, InterTics, *Pending, *W->GetMapName(),
						bBackground, (FPlatformTime::Seconds() - LoadStart) * 1000, W->Flow().Kills, PS->LevelKills, PS->Inv.Ammo[1]);
					FPlatformMisc::RequestExit(false, TEXT("exittest"));
					GetWorldTimerManager().ClearTimer(*T3); // last: this frees the lambda's captures
				}, 0.02f, true);
			}, 1.5f, false);
		}, 1.f, false);
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
				W->CompleteFinaleText();
				W->FinaleInput();
			}
			if (W->GetState() == EDoomGameState::Finale && W->GetFinale().Stage == UDoomWorld::EFinaleStage::Text)
			{
				W->CompleteFinaleText(); // whole text for the screenshot
			}
			UE_LOG(LogDoomTest, Display, TEXT("Endtest: state %d stage %d picture %s text lines %d cast %s"),
				(int32)W->GetState(), (int32)W->GetFinale().Stage, *W->GetFinale().Picture, W->GetFinale().Text.Num(), *W->CastName());
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
			const FVector Feet = P->GetActorLocation() - FVector(0, 0, Doom::HalfHeight(P)) + Fwd * 160.f * Doom::Scale;
			W->SpawnMonster(SpawnFront, Feet, Doom::YawToAngle(P->GetControlRotation().Yaw) + 180.f);
		}
	}

	// -fxtest: every particle burst in a row 200 units ahead, four times a second (modern renderer)
	if (FParse::Param(FCommandLine::Get(), TEXT("fxtest")) && W->GetMap())
	{
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [W]()
		{
			const ADoomPlayer* P = W->GetPlayer();
			if (!P)
			{
				return;
			}
			const FRotator View(0, P->GetControlRotation().Yaw, 0);
			const FVector Fwd = View.Vector(), Right = FRotationMatrix(View).GetUnitAxis(EAxis::Y);
			const int32 Kinds = (int32)EDoomParticles::Count;
			for (int32 k = 0; k < Kinds; ++k)
			{
				const FVector Where = P->GetActorLocation() + Fwd * 200.f * Doom::Scale + Right * (k - (Kinds - 1) * 0.5f) * 50.f * Doom::Scale;
				W->SpawnParticles((EDoomParticles)k, Where, -Fwd + FVector::UpVector * 0.5f);
			}
		}, 0.25f, true, 1.f);
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
				P->SetActorLocation(Doom::ToUE(Start, Floor) + FVector(0, 0, Doom::HalfHeight(P) + 1), false, nullptr, ETeleportType::TeleportPhysics);
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
						UE_LOG(LogDoomTest, Display, TEXT("Steptest t=%.2f doom(%.0f,%.0f) z=%.1f sector %d vel %.0f%s"), Now, D.X, D.Y, P->DoomZ(),
							W->GetMap()->SectorAt(D), P->GetVelocity().Size2D() / Doom::Scale,
							WatchSector >= 0 ? *FString::Printf(TEXT(" | sector %d floor %.0f"), WatchSector, W->GetMap()->Sectors[WatchSector].Floor) : TEXT(""));
					}
				}, 0.01f, true);
			}, 1.f, false);
		}
	}

	// -savename=TEXT [-savecommit]: type TEXT as slot 1's save name (key by key); commit with Enter
	FString SaveName;
	if (FParse::Value(FCommandLine::Get(), TEXT("-savename="), SaveName, false) && W->GetMap())
	{
		const bool bCommit = FParse::Param(FCommandLine::Get(), TEXT("savecommit"));
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [W, SaveName, bCommit]()
		{
			W->Menu.Open(*W, EMenuPage::Save);
			W->Menu.Item = 0;
			W->Menu.Select(*W);
			for (TCHAR C : SaveName)
			{
				const FKey Key = C == ' ' ? EKeys::SpaceBar : FChar::IsDigit(C) ? FKey(*FString::Printf(TEXT("%s"),
					*TArray<FString>{ TEXT("Zero"), TEXT("One"), TEXT("Two"), TEXT("Three"), TEXT("Four"), TEXT("Five"), TEXT("Six"), TEXT("Seven"), TEXT("Eight"), TEXT("Nine") }[C - '0']))
					: FKey(FName(FString(1, &C).ToUpper()));
				W->Menu.EditKey(*W, Key);
			}
			UE_LOG(LogDoomTest, Display, TEXT("Savename: editing %d, text \"%s\""), W->Menu.bEditing, *W->Menu.EditText);
			if (bCommit)
			{
				W->Menu.EditKey(*W, EKeys::Enter);
				W->RefreshSlots();
				UE_LOG(LogDoomTest, Display, TEXT("Savename: slot 1 now \"%s\", state %d"), *W->SlotDescription(1), (int32)W->GetState());
			}
		}, 1.f, false);
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
			UE_LOG(LogDoomTest, Display, TEXT("Killtype %d: killed %d, active movers %d"), KillType, Killed, W->NumMovers());
		}, 2.f, false);
	}

	// -hosttest / -jointest: the menu's Multiplayer page hosts a deathmatch on E1M1 with fast monsters,
	// or joins 127.0.0.1 (with -nettest the two then run the network test)
	if (FParse::Param(FCommandLine::Get(), TEXT("hosttest")) && OptMap.IsEmpty())
	{
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [W]()
		{
			W->Menu.Open(*W, EMenuPage::Multiplayer);
			W->Menu.NetRules = 1;
			W->Menu.NetMapIndex = FMath::Max(0, W->GetWad()->MapNames().IndexOfByKey(W->GetWad()->IsDoom2() ? TEXT("MAP01") : TEXT("E1M1")));
			W->Menu.bNetFast = true;
			W->Menu.Item = EMultiplayerRow::Host;
			W->Menu.Select(*W);
		}, 2.f, false);
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("jointest")))
	{
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [W]()
		{
			if (W->IsNetClient()) return;
			W->Menu.Open(*W, EMenuPage::Multiplayer);
			W->Menu.Item = EMultiplayerRow::Connect; // to 127.0.0.1
			W->Menu.Select(*W);
		}, 3.f, false);
	}
	// -perftest: a performance measure of the map (DoomPerfTest.cpp)
	if (FParse::Param(FCommandLine::Get(), TEXT("perftest")) && W->GetMap())
	{
		DoomPerfTest::Start(GetWorld());
	}
	// -editortest: the map editor's scripted test; -mapeditor: open the editor once the map is up
	if (FParse::Param(FCommandLine::Get(), TEXT("editortest")) && W->GetMap())
	{
		DoomEditorTest::Start(GetWorld());
	}
	else if (FParse::Param(FCommandLine::Get(), TEXT("mapeditor")) || UDoomMapEditor::bOpenOnNextPlay)
	{
		UDoomMapEditor::bOpenOnNextPlay = false;
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [this]() { DoomEdit(); }, 0.5f, false);
	}

	// -playtest: doors, lifts, splash damage, weapons, monsters (needs -warp); PASS/FAIL per step
	if (FParse::Param(FCommandLine::Get(), TEXT("playtest")) && W->GetMap())
	{
		StartPlayTest();
	}

	// -savetest: save mid-action, scramble, load, compare (needs -warp)
	if (FParse::Param(FCommandLine::Get(), TEXT("savetest")) && W->GetMap())
	{
		FTimerHandle T;
		GetWorldTimerManager().SetTimer(T, [W]() { W->RunSaveTest(nullptr); }, 2.f, false);
	}

	// -autotest[=seconds] [-autoyaw=deg]: screenshot Saved/Screenshots/autotest_<map>.png and quit
	FString AutoTest;
	if (FParse::Value(FCommandLine::Get(), TEXT("-autotest="), AutoTest) || FParse::Param(FCommandLine::Get(), TEXT("autotest")))
	{
		const float Delay = AutoTest.IsEmpty() ? 4.f : FCString::Atof(*AutoTest);
		// deterministic captures: stray clicks or keys on the test window must not play the game
		W->SetInputLocked(true);
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
				UE_LOG(LogDoomTest, Display, TEXT("Autotest: screenshot %s, kills %d/%d items %d/%d, %d mesh tiles, %d particle bursts | %s"), *Name, W->Flow().Kills, W->Flow().TotalKills, W->Flow().Items, W->Flow().TotalItems,
					W->GetLevel() ? W->GetLevel()->NumBakedTiles() : 0, W->GetStats().ParticleBursts, *W->StateFingerprint());
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
			P->SetGodMode(!P->IsGodMode());
			if (P->IsGodMode() && !P->IsDead())
			{
				P->Inv().Health = 100; // IDDQD heals, too
			}
			W->ShowMessage(P->IsGodMode() ? TEXT("God mode on") : TEXT("God mode off"));
		}
	}
}

void ADoomGameMode::DoomGiveAll()
{
	if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>())
	{
		if (ADoomPlayer* P = W->GetPlayer())
		{
			P->Inv().bBackpack = true;
			for (bool& B : P->Inv().Weapons) B = true;
			for (int32 i = 0; i < 4; ++i) P->Inv().Ammo[i] = DoomInfo::MaxAmmo((EDoomAmmo)i, true);
			for (bool& K : P->Inv().Keys) K = true;
			P->Inv().Armor = 200;
			P->Inv().ArmorClass = 2;
			W->ShowMessage(TEXT("All weapons, ammo and keys"));
		}
	}
}

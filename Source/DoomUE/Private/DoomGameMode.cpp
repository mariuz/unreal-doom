#include "DoomGameMode.h"
#include "DoomWorld.h"
#include "DoomPlayer.h"
#include "DoomPlayerController.h"
#include "DoomHUD.h"
#include "DoomSettings.h"
#include "DoomWad.h"
#include "DoomUE.h"
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
	}

	// -autotest[=seconds] [-autoyaw=deg]: screenshot Saved/Screenshots/autotest_<map>.png and quit
	FString AutoTest;
	if (FParse::Value(FCommandLine::Get(), TEXT("-autotest="), AutoTest) || FParse::Param(FCommandLine::Get(), TEXT("autotest")))
	{
		const float Delay = AutoTest.IsEmpty() ? 4.f : FCString::Atof(*AutoTest);
		// deterministic captures: stray clicks or keys on the test window must not play the game
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			PC->DisableInput(PC);
			PC->SetIgnoreMoveInput(true);
			PC->SetIgnoreLookInput(true);
		}
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
				UE_LOG(LogDoom, Display, TEXT("Autotest: screenshot %s, kills %d/%d items %d/%d"), *Name, W->Kills, W->TotalKills, W->Items, W->TotalItems);
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

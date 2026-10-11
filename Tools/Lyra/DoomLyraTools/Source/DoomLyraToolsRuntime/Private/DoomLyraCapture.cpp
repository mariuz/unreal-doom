#include "DoomLyraCapture.h"
#include "Modules/ModuleManager.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"
#include "EngineUtils.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, DoomLyraToolsRuntime);

bool UDoomLyraCapture::ShouldCreateSubsystem(UObject* Outer) const
{
	return FParse::Param(FCommandLine::Get(), TEXT("doomshot")) || FCommandLine::Get() && FString(FCommandLine::Get()).Contains(TEXT("-doomshot="));
}

void UDoomLyraCapture::OnWorldBeginPlay(UWorld& World)
{
	Super::OnWorldBeginPlay(World);
	if (!World.IsGameWorld() || World.GetMapName().Contains(TEXT("Entry")) || World.GetMapName().Contains(TEXT("FrontEnd")))
	{
		return;
	}
	float Shot = 30.f, Quit = 0.f;
	FParse::Value(FCommandLine::Get(), TEXT("-doomshot="), Shot);
	FParse::Value(FCommandLine::Get(), TEXT("-doomquit="), Quit);
	const FString Map = World.GetMapName();
	for (int32 i = 0; i < 2; ++i)
	{
		FTimerHandle T;
		TWeakObjectPtr<UWorld> WeakWorld(&World);
		World.GetTimerManager().SetTimer(T, [Map, i, WeakWorld]()
		{
			if (UWorld* W = WeakWorld.Get())
			{
				int32 Pawns = 0;
				for (TActorIterator<APawn> It(W); It; ++It)
				{
					++Pawns;
					UE_LOG(LogTemp, Display, TEXT("DoomLyraCapture: pawn %s at %s"), *It->GetName(), *It->GetActorLocation().ToString());
				}
				APlayerController* PC = W->GetFirstPlayerController();
				if (PC && PC->PlayerCameraManager)
				{
					UE_LOG(LogTemp, Display, TEXT("DoomLyraCapture: camera at %s looking %s, %d pawns"),
						*PC->PlayerCameraManager->GetCameraLocation().ToString(), *PC->PlayerCameraManager->GetCameraRotation().ToString(), Pawns);
				}
			}
			const FString Name = FString::Printf(TEXT("DoomLyra_%s_%d.png"), *Map, i + 1);
			FScreenshotRequest::RequestScreenshot(FPaths::ScreenShotDir() / Name, true, false);
			UE_LOG(LogTemp, Display, TEXT("DoomLyraCapture: screenshot %s"), *Name);
		}, Shot + i * 15.f, false);
	}
	if (Quit > 0.f)
	{
		FTimerHandle Q;
		World.GetTimerManager().SetTimer(Q, []() { FPlatformMisc::RequestExit(false, TEXT("DoomLyraCapture")); }, Quit, false);
	}
}

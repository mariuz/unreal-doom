// -perftest (with -warp): a performance measure of the map, for the roadmap's "measure first".
// After the level settles: 8 s with the monsters as the map placed them (asleep, waiting for the
// player), then 8 s with every monster awake and after the player (god mode). Logged per phase:
// the game thread's time a frame, the frame time, Doom sight checks a second, how many monsters run
// an actor tick and how many AI controllers exist.
#include "DoomWorld.h"
#include "DoomMonster.h"
#include "DoomPlayer.h"
#include "DoomInfo.h"
#include "DoomUE.h"
#include "AIController.h"
#include "EngineUtils.h"
#include "RenderTimer.h"
#include "Containers/Ticker.h"

namespace DoomPerfTest
{
	struct FPhase
	{
		double GameMs = 0, FrameMs = 0;
		TArray<float> GameFrames; // per frame, for the median (hitches such as shader compiles skew the mean)
		int32 Frames = 0;
		int64 SightStart = 0, RejectStart = 0, SpawnStart = 0, ReuseStart = 0;
		double Seconds = 0;
	};

	struct FRun
	{
		TWeakObjectPtr<UWorld> World;
		int32 Stage = 0;
		double Time = 0;
		FPhase Phase;
	};

	FString Counts(UWorld* World, UDoomWorld* W)
	{
		int32 Monsters = 0, Ticking = 0, Moving = 0, Controllers = 0;
		for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
		{
			if (!M.IsValid() || M->IsDead()) continue;
			++Monsters;
			Ticking += M->IsActorTickEnabled() ? 1 : 0;
			Moving += M->GetCharacterMovement() && M->GetCharacterMovement()->IsComponentTickEnabled() ? 1 : 0;
		}
		for (TActorIterator<AAIController> It(World); It; ++It) ++Controllers;
		return FString::Printf(TEXT("%d live monsters, %d with an actor tick, %d with a movement tick, %d AI controllers"), Monsters, Ticking, Moving, Controllers);
	}

	void Report(FRun& R, UWorld* World, UDoomWorld* W, const TCHAR* Name)
	{
		const FPhase& P = R.Phase;
		const int32 N = FMath::Max(1, P.Frames);
		const int64 Sights = W->SightChecks - P.SightStart;
		TArray<float> Sorted = P.GameFrames;
		Sorted.Sort();
		const float Median = Sorted.Num() ? Sorted[Sorted.Num() / 2] : 0.f;
		UE_LOG(LogDoom, Display, TEXT("Perftest %s %s: game thread median %.2f ms (mean %.2f), frame %.2f ms (%d frames), %.0f sight checks a second (%.0f%% by REJECT), effects %lld new and %lld reused; %s"),
			*W->GetMapName(), Name, Median, P.GameMs / N, P.FrameMs / N, P.Frames, Sights / FMath::Max(0.001, P.Seconds),
			Sights ? 100.0 * (W->RejectedSights - P.RejectStart) / Sights : 0.0, W->EffectsSpawned - P.SpawnStart, W->EffectsReused - P.ReuseStart, *Counts(World, W));
	}

	bool Tick(FRun& R, float Dt)
	{
		UWorld* World = R.World.Get();
		UDoomWorld* W = World ? World->GetSubsystem<UDoomWorld>() : nullptr;
		if (!W || !W->GetMap())
		{
			return false;
		}
		R.Time += Dt;
		if (R.Stage >= 2)
		{
			if (ADoomPlayer* P = W->GetPlayer())
			{
				P->Inv().Ammo[(int32)EDoomAmmo::Bullets] = 200;
				P->SetActorRotation(FRotator::ZeroRotator);
				P->SetFireHeld(true);
			}
		}
		if (R.Stage == 1 || R.Stage == 3)
		{
			R.Phase.GameMs += FPlatformTime::ToMilliseconds(GGameThreadTime);
			R.Phase.GameFrames.Add(FPlatformTime::ToMilliseconds(GGameThreadTime));
			R.Phase.FrameMs += Dt * 1000.0;
			R.Phase.Seconds += Dt;
			++R.Phase.Frames;
		}
		auto Begin = [&](int32 Stage)
		{
			R.Stage = Stage;
			R.Time = 0;
			R.Phase = FPhase();
			R.Phase.SightStart = W->SightChecks;
			R.Phase.RejectStart = W->RejectedSights;
			R.Phase.SpawnStart = W->EffectsSpawned;
			R.Phase.ReuseStart = W->EffectsReused;
		};
		switch (R.Stage)
		{
		case 0:
			if (R.Time > 4.0) Begin(1);
			break;
		case 1:
			if (R.Time > 8.0)
			{
				Report(R, World, W, TEXT("asleep"));
				// everyone after the player
				if (ADoomPlayer* P = W->GetPlayer())
				{
					P->bGodMode = true;
					// and the chaingun firing all the while: puffs and blood by the dozen
					for (bool& B : P->Inv().Weapons) B = true;
					P->PendingWeapon = EDoomWeapon::Chaingun;
					P->SetFireHeld(true);
					for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
					{
						if (M.IsValid() && !M->IsDead()) M->WakeUp(P, false);
					}
				}
				Begin(2);
			}
			break;
		case 2:
			if (R.Time > 1.0) Begin(3);
			break;
		case 3:
			if (R.Time > 8.0)
			{
				Report(R, World, W, TEXT("awake"));
				FPlatformMisc::RequestExit(false);
				return false;
			}
			break;
		}
		return true;
	}

	void Start(UWorld* World)
	{
		TSharedRef<FRun> R = MakeShared<FRun>();
		R->World = World;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([R](float Dt) { return Tick(*R, Dt); }));
	}
}

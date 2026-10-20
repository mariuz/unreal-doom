// -perftest (with -warp): a performance measure of the map, for the roadmap's "measure first".
// After the level settles: 8 s with the monsters as the map placed them (asleep, waiting for the
// player), 4 s with every door and lift moving, then 8 s with every monster awake and after the
// player (god mode, firing the chaingun). Logged per phase:
// the game thread's time a frame, the frame time, Doom sight checks a second, how many monsters run
// an actor tick and how many AI controllers exist.
#include "DoomWorld.h"
#include "DoomMonster.h"
#include "DoomPlayer.h"
#include "DoomInfo.h"
#include "DoomLevel.h"
#include "DoomUE.h"
#include "AIController.h"
#include "EngineUtils.h"
#include "RenderTimer.h"
#include "RHI.h"
#include "Containers/Ticker.h"

namespace DoomPerfTest
{
	struct FPhase
	{
		double GameMs = 0, FrameMs = 0;
		TArray<float> GameFrames; // per frame, for the median (hitches such as shader compiles skew the mean)
		TArray<float> RenderFrames, GpuFrames;
		int32 Frames = 0;
		int64 SightStart = 0, RejectStart = 0, SpawnStart = 0, ReuseStart = 0, ChunkStart = 0;
		double FlushStart = 0;
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
		auto MedianOf = [](TArray<float> A) { A.Sort(); return A.Num() ? A[A.Num() / 2] : 0.f; };
		UE_LOG(LogDoom, Display, TEXT("Perftest %s %s: render thread median %.2f ms, GPU median %.2f ms"), *W->GetMapName(), Name,
			MedianOf(P.RenderFrames), MedianOf(P.GpuFrames));
		const ADoomLevel* Lv = W->GetLevel();
		UE_LOG(LogDoom, Display, TEXT("Perftest %s %s: moving chunks %lld regenerated, %.2f ms a frame"), *W->GetMapName(), Name,
			Lv ? Lv->ChunksRegenerated - P.ChunkStart : 0, Lv ? (Lv->FlushSeconds - P.FlushStart) * 1000.0 / N : 0.0);
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
		if (R.Stage >= 2 && R.Stage < 4)
		{
			if (ADoomPlayer* P = W->GetPlayer())
			{
				P->Inv().Ammo[(int32)EDoomAmmo::Bullets] = 200;
				P->SetActorRotation(FRotator::ZeroRotator);
				P->SetFireHeld(true);
			}
		}
		if (R.Stage == 1 || R.Stage == 3 || R.Stage == 4)
		{
			R.Phase.GameMs += FPlatformTime::ToMilliseconds(GGameThreadTime);
			R.Phase.GameFrames.Add(FPlatformTime::ToMilliseconds(GGameThreadTime));
			R.Phase.RenderFrames.Add(FPlatformTime::ToMilliseconds(GRenderThreadTime));
			R.Phase.GpuFrames.Add(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()));
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
			R.Phase.ChunkStart = W->GetLevel() ? W->GetLevel()->ChunksRegenerated : 0;
			R.Phase.FlushStart = W->GetLevel() ? W->GetLevel()->FlushSeconds : 0;
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
				// every door opening and every lift going down at once, the monsters still waiting
				if (ADoomPlayer* P = W->GetPlayer())
				{
					int32 Started = 0;
					static const int32 Movers[] = { 1, 26, 27, 28, 31, 32, 33, 34, 117, 118, 62, 88, 120, 123, 21, 10 };
					for (int32 l = 0; l < W->GetMap()->Lines.Num(); ++l)
					{
						const FDoomLine& L = W->GetMap()->Lines[l];
						bool bMover = false;
						for (int32 S : Movers) bMover |= L.Special == S;
						if (bMover && L.Side[1] != INDEX_NONE) Started += W->UseLine(l, P) ? 1 : 0;
					}
					UE_LOG(LogDoom, Display, TEXT("Perftest %s: started %d doors and lifts"), *W->GetMapName(), Started);
				}
				Begin(4);
			}
			break;
		case 4:
			if (R.Time > 4.0)
			{
				Report(R, World, W, TEXT("movers"));
				// then everyone after the player, and the chaingun firing all the while
				if (ADoomPlayer* P = W->GetPlayer())
				{
					P->bGodMode = true;
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

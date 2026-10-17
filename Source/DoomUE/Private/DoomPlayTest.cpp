// -playtest: a scripted play-through of the interactive features, step by step, each logged as
// "Playtest <step> PASS/FAIL/SKIP: details", then "Playtest: N passed, M failed, K skipped" and quit.
// Needs -warp (E1M1 of Freedoom 1 is the reference map). Steps that need a feature the map doesn't
// have (a lift, a teleporter) are skipped. Screenshots land in Saved/Screenshots/.../playtest_*.png.
#include "DoomGameMode.h"
#include "DoomWorld.h"
#include "DoomMap.h"
#include "DoomPlayer.h"
#include "DoomMonster.h"
#include "DoomUE.h"
#include "Engine/DamageEvents.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "TimerManager.h"

namespace
{
	enum class EStep : uint8 { Running, Pass, Fail, Skip };

	struct FPlayStep
	{
		FString Name;
		/** Sets the step up; returns false (with a reason in Msg) to skip it. */
		TFunction<bool(FString& Msg)> Start;
		/** Called every tic with the tics since Start; Running until it decides. */
		TFunction<EStep(int32 Tics, FString& Msg)> Poll;
	};

	struct FPlayRun
	{
		TArray<FPlayStep> Steps;
		int32 Index = -1;
		int32 StartTic = 0;
		int32 LastTic = -1;
		bool bStarted = false;
		int32 Passed = 0, Failed = 0, Skipped = 0;
		FTimerHandle Timer;
	};
}

void ADoomGameMode::StartPlayTest()
{
	UDoomWorld* W = DoomWorld();
	TSharedRef<FPlayRun> Run = MakeShared<FPlayRun>();
	W->bInputLocked = true; // stray keys on the test window must not play

	auto Player = [W]() { return W->GetPlayer(); };
	// puts the player back at the map's start, healthy and still, so steps don't depend on each other
	TSharedRef<FVector> Home = MakeShared<FVector>(FVector::ZeroVector);
	TSharedRef<FRotator> HomeView = MakeShared<FRotator>(FRotator::ZeroRotator);
	auto GoHome = [W, Home, HomeView]()
	{
		ADoomPlayer* P = W->GetPlayer();
		P->SetActorLocation(*Home, false, nullptr, ETeleportType::TeleportPhysics);
		P->GetCharacterMovement()->StopMovementImmediately();
		P->GetController()->SetControlRotation(*HomeView);
		P->Inv().Health = 100;
		P->Inv().Armor = 0;
		P->Inv().ArmorClass = 0;
		P->bGodMode = false;
		P->bFireHeld = false;
		P->ReactionTime = 0;
	};
	auto Shot = [](const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ScreenShotDir() / FString::Printf(TEXT("playtest_%s.png"), Name), true, false);
	};
	FDoomMap* Map = W->GetMap();
	auto FindLine = [Map](std::initializer_list<int32> Specials, bool bNeedBack, bool bNeedTag) -> int32
	{
		for (int32 i = 0; i < Map->Lines.Num(); ++i)
		{
			const FDoomLine& L = Map->Lines[i];
			for (int32 S : Specials)
			{
				if (L.Special == S && (!bNeedBack || L.BackSector != INDEX_NONE) && (!bNeedTag || Map->SectorsWithTag(L.Tag).Num() > 0))
				{
					return i;
				}
			}
		}
		return INDEX_NONE;
	};

	// --- 1. a door opens, waits and closes again (DR, special 1) ---------------------------------
	{
		struct FDoor { int32 Line = -1, Sector = -1; float Floor = 0, Top = 0, MaxCeiling = -1e9f; };
		TSharedRef<FDoor> D = MakeShared<FDoor>();
		Run->Steps.Add({ TEXT("door"),
			[W, Map, D, FindLine, Player](FString& Msg)
			{
				D->Line = FindLine({ 1 }, true, false);
				if (D->Line < 0) { Msg = TEXT("no DR door (special 1)"); return false; }
				D->Sector = Map->Lines[D->Line].BackSector;
				D->Floor = Map->Sectors[D->Sector].Floor;
				D->Top = Map->LowestCeilingSurrounding(D->Sector) - 4.f;
				return W->ActivateSpecial(D->Line, 0, Player(), EActivation::Use, true);
			},
			[W, Map, D, Shot](int32 Tics, FString& Msg)
			{
				const FDoomSector& S = Map->Sectors[D->Sector];
				D->MaxCeiling = FMath::Max(D->MaxCeiling, S.Ceiling);
				Msg = FString::Printf(TEXT("line %d sector %d: floor %.0f, opened to %.0f (want %.0f), now %.0f after %d tics, mover %d"),
					D->Line, D->Sector, D->Floor, D->MaxCeiling, D->Top, S.Ceiling, Tics, W->SectorHasMover(D->Sector));
				if (!W->SectorHasMover(D->Sector))
				{
					// open to the top, wait ~150 tics, close: the whole cycle takes well over 150 tics
					return D->MaxCeiling >= D->Top - 0.5f && FMath::IsNearlyEqual(S.Ceiling, D->Floor, 0.5f) && Tics > 150 ? EStep::Pass : EStep::Fail;
				}
				return Tics > 700 ? EStep::Fail : EStep::Running;
			} });
	}

	// --- 2. a lift lowers, waits and comes back up (SR/WR/S1/W1 lift) -----------------------------
	{
		struct FLift { int32 Line = -1, Sector = -1; float Start = 0, MinFloor = 1e9f; };
		TSharedRef<FLift> L = MakeShared<FLift>();
		Run->Steps.Add({ TEXT("lift"),
			[W, Map, L, FindLine, Player](FString& Msg)
			{
				L->Line = FindLine({ 62, 88, 10, 21, 120, 121, 122, 123 }, false, true);
				if (L->Line < 0) { Msg = TEXT("no lift line"); return false; }
				L->Sector = Map->SectorsWithTag(Map->Lines[L->Line].Tag)[0];
				L->Start = Map->Sectors[L->Sector].Floor;
				return W->ActivateSpecial(L->Line, 0, Player(), EActivation::Use, true);
			},
			[W, Map, L](int32 Tics, FString& Msg)
			{
				const FDoomSector& S = Map->Sectors[L->Sector];
				L->MinFloor = FMath::Min(L->MinFloor, S.Floor);
				Msg = FString::Printf(TEXT("line %d sector %d: floor %.0f, lowered to %.0f, now %.0f after %d tics"),
					L->Line, L->Sector, L->Start, L->MinFloor, S.Floor, Tics);
				if (!W->SectorHasMover(L->Sector))
				{
					return L->MinFloor < L->Start - 1.f && FMath::IsNearlyEqual(S.Floor, L->Start, 0.5f) && Tics > 105 ? EStep::Pass : EStep::Fail;
				}
				return Tics > 700 ? EStep::Fail : EStep::Running;
			} });
	}

	// --- 3. splash damage falls off with distance (P_RadiusAttack) --------------------------------
	{
		struct FSplash { int32 Near = 0, Far = 0, WantNear = 0; float Pushed = 0; };
		TSharedRef<FSplash> B = MakeShared<FSplash>();
		Run->Steps.Add({ TEXT("splash"),
			[W, B, GoHome, Player](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				const FVector Fwd = FRotator(0, P->GetControlRotation().Yaw, 0).Vector();
				// 128 damage, 40 units ahead: 128 - (40 - 16 player radius) = 104 (halved on skill 1)
				B->WantNear = W->GetSkill() == 1 ? 52 : 104;
				W->RadiusAttack(nullptr, nullptr, P->GetActorLocation() + Fwd * 40.f * Doom::Scale, 128.f);
				B->Near = 100 - P->Inv().Health;
				B->Pushed = P->GetCharacterMovement()->Velocity.Size2D() / Doom::Scale;
				GoHome();
				// 160 units ahead: out of range
				W->RadiusAttack(nullptr, nullptr, P->GetActorLocation() + Fwd * 160.f * Doom::Scale, 128.f);
				B->Far = 100 - P->Inv().Health;
				GoHome();
				return true;
			},
			[B](int32 Tics, FString& Msg)
			{
				Msg = FString::Printf(TEXT("40 units: %d damage (want %d), 160 units: %d (want 0); no knockback without an inflictor (%.0f u/s)"),
					B->Near, B->WantNear, B->Far, B->Pushed);
				return B->Near == B->WantNear && B->Far == 0 ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 4. weapons: fire rates with the trigger held, and damage on a target --------------------
	{
		struct FGun { EDoomWeapon Weapon; int32 HoldTics; int32 MinShots; int32 MaxShots; };
		// vanilla: pistol every 14 tics, chaingun 2 per 8, plasma every 3, shotgun every 37, rockets every 20
		const TArray<FGun> Guns = {
			{ EDoomWeapon::Pistol, 70, 5, 6 }, { EDoomWeapon::Chaingun, 70, 17, 19 }, { EDoomWeapon::Plasma, 70, 23, 24 },
			{ EDoomWeapon::Shotgun, 111, 3, 4 }, { EDoomWeapon::Rocket, 60, 3, 4 } };
		for (const FGun& G : Guns)
		{
			struct FRate { int32 Ammo0 = 0; int32 FireTic = -1; int32 Shots = 0; };
			TSharedRef<FRate> R = MakeShared<FRate>();
			const FString Name = FString::Printf(TEXT("rate_%s"), *UEnum::GetDisplayValueAsText(G.Weapon).ToString());
			Run->Steps.Add({ Name,
				[W, G, R, GoHome, Player](FString& Msg)
				{
					GoHome();
					ADoomPlayer* P = Player();
					P->bGodMode = true;
					for (bool& B : P->Inv().Weapons) B = true;
					P->Inv().bBackpack = true;
					for (int32 i = 0; i < 4; ++i) P->Inv().Ammo[i] = DoomInfo::MaxAmmo((EDoomAmmo)i, true);
					// face up into the air so rockets fly off instead of blowing up at our feet
					P->GetController()->SetControlRotation(FRotator(60.f, P->GetControlRotation().Yaw, 0));
					P->PendingWeapon = G.Weapon;
					R->FireTic = -1;
					return true;
				},
				[W, G, R, Player](int32 Tics, FString& Msg)
				{
					ADoomPlayer* P = Player();
					const FDoomWeaponDef& D = DoomInfo::Weapon(G.Weapon);
					const int32 Ammo = D.Ammo == EDoomAmmo::None ? 0 : P->Inv().Ammo[(int32)D.Ammo];
					if (R->FireTic < 0)
					{
						// wait for the weapon to come up, then hold the trigger
						if (P->Inv().Ready == G.Weapon && P->Phase == ADoomPlayer::EWeaponPhase::Ready)
						{
							R->FireTic = Tics;
							R->Ammo0 = Ammo;
							P->bFireHeld = true;
						}
						return Tics > 200 ? EStep::Fail : EStep::Running;
					}
					R->Shots = (R->Ammo0 - Ammo) / FMath::Max(1, D.AmmoPerShot);
					Msg = FString::Printf(TEXT("%d shots in %d tics (want %d-%d)"), R->Shots, Tics - R->FireTic, G.MinShots, G.MaxShots);
					if (Tics - R->FireTic >= G.HoldTics)
					{
						P->bFireHeld = false;
						return R->Shots >= G.MinShots && R->Shots <= G.MaxShots ? EStep::Pass : EStep::Fail;
					}
					return EStep::Running;
				} });
		}
	}

	// --- 5. a cacodemon stays airborne after being hit --------------------------------------------
	{
		TSharedRef<TWeakObjectPtr<ADoomMonster>> Caco = MakeShared<TWeakObjectPtr<ADoomMonster>>();
		Run->Steps.Add({ TEXT("flyer"),
			[W, Caco, GoHome, Player](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				P->bGodMode = true;
				const FVector Fwd = FRotator(0, P->GetControlRotation().Yaw, 0).Vector();
				const FVector Feet = P->GetActorLocation() - FVector(0, 0, Doom::HalfHeight(P)) + Fwd * 96.f * Doom::Scale;
				*Caco = W->SpawnMonster(3005, Feet, Doom::YawToAngle(P->GetControlRotation().Yaw) + 180.f);
				if (!Caco->IsValid()) { Msg = TEXT("could not spawn a cacodemon"); return false; }
				UGameplayStatics::ApplyDamage(Caco->Get(), 10.f, P->GetController(), P, nullptr);
				return true;
			},
			[Caco](int32 Tics, FString& Msg)
			{
				if (!Caco->IsValid()) { Msg = TEXT("cacodemon vanished"); return EStep::Fail; }
				const EMovementMode Mode = (*Caco)->GetCharacterMovement()->MovementMode;
				Msg = FString::Printf(TEXT("movement mode %d after %d tics (flying = %d), health %d"), (int32)Mode, Tics, (int32)MOVE_Flying, (*Caco)->Health);
				if (Tics < 70) return EStep::Running;
				const bool bPass = Mode == MOVE_Flying;
				UGameplayStatics::ApplyDamage(Caco->Get(), 100000.f, nullptr, nullptr, nullptr); // out of the way
				return bPass ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 6. rockets and plasma hurt a monster in front of the player ------------------------------
	{
		struct FTarget { TWeakObjectPtr<ADoomMonster> M; int32 Health0 = 0; };
		TSharedRef<FTarget> T = MakeShared<FTarget>();
		Run->Steps.Add({ TEXT("target"),
			[W, T, GoHome, Player, Shot](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				P->bGodMode = true;
				const FVector Fwd = FRotator(0, P->GetControlRotation().Yaw, 0).Vector();
				const FVector Feet = P->GetActorLocation() - FVector(0, 0, Doom::HalfHeight(P)) + Fwd * 128.f * Doom::Scale;
				T->M = W->SpawnMonster(3003, Feet, Doom::YawToAngle(P->GetControlRotation().Yaw) + 180.f); // a baron: 1000 health
				if (!T->M.IsValid()) { Msg = TEXT("could not spawn a baron"); return false; }
				T->Health0 = T->M->Health;
				P->PendingWeapon = EDoomWeapon::Plasma;
				return true;
			},
			[W, T, Player, Shot](int32 Tics, FString& Msg)
			{
				ADoomPlayer* P = Player();
				if (!T->M.IsValid()) { Msg = TEXT("baron vanished"); return EStep::Fail; }
				P->bFireHeld = P->Inv().Ready == EDoomWeapon::Plasma;
				if (Tics == 60) Shot(TEXT("plasma"));
				Msg = FString::Printf(TEXT("baron health %d -> %d after %d tics of plasma"), T->Health0, T->M->Health, Tics);
				if (Tics < 105) return EStep::Running;
				P->bFireHeld = false;
				const bool bPass = T->M->Health < T->Health0 - 100;
				UGameplayStatics::ApplyDamage(T->M.Get(), 100000.f, nullptr, nullptr, nullptr);
				return bPass ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 7. awake monsters actually walk (no freezing in low rooms or doorways) -------------------
	{
		struct FChase { TMap<TWeakObjectPtr<ADoomMonster>, FVector2f> Start; };
		TSharedRef<FChase> C = MakeShared<FChase>();
		Run->Steps.Add({ TEXT("chase"),
			[W, C, GoHome, Player](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				P->bGodMode = true;
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
				{
					if (M.IsValid() && !M->IsDead() && M->Def->Speed > 0)
					{
						M->Target = P;
						M->StartState(EMonsterState::Chase);
						C->Start.Add(M, M->DoomPos());
					}
				}
				if (C->Start.Num() == 0) { Msg = TEXT("no monsters"); return false; }
				return true;
			},
			[C](int32 Tics, FString& Msg)
			{
				if (Tics < 175) return EStep::Running;
				int32 Moved = 0, Alive = 0;
				FString Stuck;
				for (const TPair<TWeakObjectPtr<ADoomMonster>, FVector2f>& E : C->Start)
				{
					if (!E.Key.IsValid() || E.Key->IsDead()) continue;
					++Alive;
					if (FVector2f::Distance(E.Key->DoomPos(), E.Value) > 32.f) ++Moved;
					else Stuck += FString::Printf(TEXT(" %s(%.0f,%.0f)"), *E.Key->Def->Name, E.Value.X, E.Value.Y);
				}
				Msg = FString::Printf(TEXT("%d of %d awake monsters moved over 32 units in 5 s; still:%s"), Moved, Alive, *Stuck);
				return Alive > 0 && Moved * 10 >= Alive * 8 ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 8. a teleporter moves the player to its destination and nothing else fires --------------
	{
		struct FTele { int32 Line = -1; FString Map; };
		TSharedRef<FTele> T = MakeShared<FTele>();
		Run->Steps.Add({ TEXT("teleport"),
			[W, Map, T, FindLine, GoHome, Player](FString& Msg)
			{
				T->Line = FindLine({ 97, 39 }, false, true);
				if (T->Line < 0) { Msg = TEXT("no teleporter (39/97)"); return false; }
				GoHome();
				T->Map = W->GetMapName();
				return W->ActivateSpecial(T->Line, 0, Player(), EActivation::Cross, true);
			},
			[W, Map, T, Player](int32 Tics, FString& Msg)
			{
				ADoomPlayer* P = Player();
				const int32 Sec = Map->SectorAt(P->DoomPos());
				const int32 Tag = Map->Lines[T->Line].Tag;
				Msg = FString::Printf(TEXT("line %d tag %d: player in sector %d (tag %d), frozen %d tics, still on %s"),
					T->Line, Tag, Sec, Sec >= 0 ? Map->Sectors[Sec].Tag : -1, P->ReactionTime, *W->GetMapName());
				if (Tics < 5) return EStep::Running;
				return Sec >= 0 && Map->Sectors[Sec].Tag == Tag && W->GetMapName() == T->Map && W->GetState() == EDoomGameState::Playing ? EStep::Pass : EStep::Fail;
			} });
	}

	// run the steps one after the other, one poll per Doom tic
	TSharedRef<FPlayRun> R = Run;
	GetWorldTimerManager().SetTimer(Run->Timer, [this, W, R, Home, HomeView, GoHome]()
	{
		ADoomPlayer* P = W->GetPlayer();
		if (!P || W->GetState() != EDoomGameState::Playing)
		{
			return;
		}
		const int32 Now = W->GetTic();
		if (Now == R->LastTic)
		{
			return;
		}
		R->LastTic = Now;
		if (R->Index < 0)
		{
			*Home = P->GetActorLocation();
			*HomeView = P->GetControlRotation();
			R->Index = 0;
		}
		while (R->Index < R->Steps.Num())
		{
			FPlayStep& S = R->Steps[R->Index];
			FString Msg;
			if (!R->bStarted)
			{
				R->bStarted = true;
				R->StartTic = Now;
				if (!S.Start(Msg))
				{
					UE_LOG(LogDoom, Display, TEXT("Playtest %s SKIP: %s"), *S.Name, *Msg);
					++R->Skipped;
					++R->Index;
					R->bStarted = false;
					continue;
				}
				return;
			}
			const EStep Result = S.Poll(Now - R->StartTic, Msg);
			if (Result == EStep::Running)
			{
				return;
			}
			UE_LOG(LogDoom, Display, TEXT("Playtest %s %s: %s"), *S.Name, Result == EStep::Pass ? TEXT("PASS") : TEXT("FAIL"), *Msg);
			(Result == EStep::Pass ? R->Passed : R->Failed)++;
			++R->Index;
			R->bStarted = false;
			GoHome();
		}
		UE_LOG(LogDoom, Display, TEXT("Playtest: %d passed, %d failed, %d skipped"), R->Passed, R->Failed, R->Skipped);
		FPlatformMisc::RequestExit(false, TEXT("playtest"));
		GetWorldTimerManager().ClearTimer(R->Timer); // last: this frees the lambda's captures
	}, 0.005f, true, 1.f);
}

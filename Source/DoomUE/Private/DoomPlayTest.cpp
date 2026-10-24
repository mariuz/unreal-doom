// -playtest: a scripted play-through of the interactive features, step by step, each logged as
// "Playtest <step> PASS/FAIL/SKIP: details", then "Playtest: N passed, M failed, K skipped" and quit.
// Needs -warp (E1M1 of Freedoom 1 is the reference map). Steps that need a feature the map doesn't
// have (a lift, a teleporter) are skipped. Screenshots land in Saved/Screenshots/.../playtest_*.png.
#include "DoomGameMode.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "DoomWorld.h"
#include "DoomAI.h"
#include "BrainComponent.h"
#include "DoomMap.h"
#include "DoomPlayer.h"
#include "DoomPlayerController.h"
#include "DoomMenu.h"
#include "DoomHUD.h"
#include "DoomSpriteComponent.h"
#include "DoomThing.h"
#include "DoomInput.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputKeyEventArgs.h"
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
		/** Movement steps: push forward every frame (input is used up each frame). */
		bool bHoldForward = false;
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
	// the clearest of 16 directions from where the player stands, and how far it is clear
	auto ClearestYaw = [W](ADoomPlayer* P, float& OutDist)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomPlaytest), false, P);
		float BestYaw = P->GetControlRotation().Yaw;
		OutDist = 0;
		for (int32 a = 0; a < 16; ++a)
		{
			const FVector Dir = FRotator(0, a * 22.5f, 0).Vector();
			FHitResult Hit;
			// a 16-unit ball at chest height: clear of the floor and of low steps
			const FVector From = P->GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
			const float Dist = W->GetWorld()->SweepSingleByChannel(Hit, From, From + Dir * 1000.f * Doom::Scale, FQuat::Identity, ECC_Pawn,
				FCollisionShape::MakeSphere(16.f * Doom::Scale), Q) ? Hit.Distance / Doom::Scale : 1000.f;
			if (Dist > OutDist) { OutDist = Dist; BestYaw = a * 22.5f; }
		}
		return BestYaw;
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

	// --- 3. a teleporter moves the player there and nothing else fires (early: monsters use W1 ones up)
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


	// --- 4. splash damage falls off with distance (P_RadiusAttack) --------------------------------
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

	// --- 5. walking: top speed and slide (before shots wake the monsters) -------------------------------
	{
		struct FWalk { float Top = 0; FVector2f Release; float Slide = -1; int32 ReleaseTic = -1; FRotator Dir; };
		TSharedRef<FWalk> K = MakeShared<FWalk>();
		TSharedRef<FPlayRun> RunRef = Run;
		Run->Steps.Add({ TEXT("walk"),
			[W, K, RunRef, GoHome, Player](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				// a long straight run: from any thing's spot on the map, the clearest of 16 directions
				// (800 units on the flat are enough to reach top speed and slide out)
				float Best = 0;
				FVector BestFrom = P->GetActorLocation();
				FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomPlaytest), false, P);
				const FDoomMap* M = W->GetMap();
				const float Half = Doom::HalfHeight(P);
				for (const FDoomMapThing& T : M->Things)
				{
					const int32 Sec = M->SectorAt(T.P);
					if (Sec < 0) continue;
					const FVector From = Doom::ToUE(T.P, M->Sectors[Sec].Floor) + FVector(0, 0, Half + 2.f);
					for (int32 a = 0; a < 16 && Best < 1000.f; ++a)
					{
						const FRotator Dir(0, a * 22.5f, 0);
						FHitResult Hit;
						const FVector To = From + Dir.Vector() * 1000.f * Doom::Scale;
						if (W->GetWorld()->SweepSingleByChannel(Hit, From, From, FQuat::Identity, ECC_Pawn, P->GetCapsuleComponent()->GetCollisionShape(), Q)) break; // spot taken
						const float Dist = W->GetWorld()->SweepSingleByChannel(Hit, From, To, FQuat::Identity, ECC_Pawn, P->GetCapsuleComponent()->GetCollisionShape(), Q)
							? Hit.Distance / Doom::Scale : 1000.f;
						// the floor must stay level all the way (a step or a drop changes the speed)
						FHitResult Floor;
						const FVector End = From + Dir.Vector() * Dist * Doom::Scale * 0.95f;
						const bool bLevel = W->GetWorld()->LineTraceSingleByChannel(Floor, End, End - FVector(0, 0, Half + 8.f * Doom::Scale), ECC_Visibility, Q)
							&& FMath::Abs(Floor.ImpactPoint.Z - (From.Z - Half - 2.f)) < 2.f * Doom::Scale;
						if (bLevel && Dist > Best) { Best = Dist; K->Dir = Dir; BestFrom = From; }
					}
				}
				if (Best < 800.f) { Msg = FString::Printf(TEXT("no 800-unit level run on the map (longest %.0f)"), Best); return false; }
				P->SetActorLocation(BestFrom, false, nullptr, ETeleportType::TeleportPhysics);
				P->GetController()->SetControlRotation(K->Dir);
				RunRef->bHoldForward = true;
				return true;
			},
			[K, RunRef, Player](int32 Tics, FString& Msg)
			{
				ADoomPlayer* P = Player();
				const float Speed = P->GetVelocity().Size2D() / Doom::Scale / Doom::TicRate;
				if (Tics <= 30)
				{
					K->Top = FMath::Max(K->Top, Speed);
					return EStep::Running;
				}
				if (K->ReleaseTic < 0)
				{
					RunRef->bHoldForward = false;
					K->ReleaseTic = Tics;
					K->Release = P->DoomPos();
					return EStep::Running;
				}
				if (Speed > 0.01f && Tics - K->ReleaseTic < 140) return EStep::Running;
				K->Slide = FVector2f::Distance(P->DoomPos(), K->Release);
				Msg = FString::Printf(TEXT("top speed %.1f units/tic after 30 tics (vanilla 16.7, ~15.5 by then), slid %.0f units after letting go (vanilla ~160)"), K->Top, K->Slide);
				return K->Top > 14.f && K->Top < 17.5f && K->Slide > 110.f && K->Slide < 210.f ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 6. weapons: fire rates with the trigger held, and damage on a target --------------------
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

	// --- 7. a cacodemon stays airborne after being hit --------------------------------------------
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

	// --- 8. plasma hurts a monster in front of the player -----------------------------------------
	{
		struct FTarget { TWeakObjectPtr<ADoomMonster> M; int32 Health0 = 0; };
		TSharedRef<FTarget> T = MakeShared<FTarget>();
		Run->Steps.Add({ TEXT("target"),
			[W, T, GoHome, Player, ClearestYaw](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				P->bGodMode = true;
				// face the open side of the start, so the baron stands in the open
				float Clear = 0;
				const float Yaw = ClearestYaw(P, Clear);
				if (Clear < 200.f) { Msg = FString::Printf(TEXT("no room for a baron at the start (clear %.0f)"), Clear); return false; }
				P->GetController()->SetControlRotation(FRotator(0, Yaw, 0));
				const FVector Fwd = FRotator(0, Yaw, 0).Vector();
				const FVector Feet = P->GetActorLocation() - FVector(0, 0, Doom::HalfHeight(P)) + Fwd * 128.f * Doom::Scale;
				T->M = W->SpawnMonster(3003, Feet, Doom::YawToAngle(Yaw) + 180.f); // a baron: 1000 health
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
				// keep it in the sights, as a player would (it walks up for a swipe)
				const FVector ToBaron = T->M->GetActorLocation() - P->GetActorLocation();
				P->GetController()->SetControlRotation(FRotator(0, ToBaron.Rotation().Yaw, 0));
				if (Tics == 60) Shot(TEXT("plasma"));
				Msg = FString::Printf(TEXT("baron health %d -> %d after %d tics of plasma"), T->Health0, T->M->Health, Tics);
				if (Tics < 105) return EStep::Running;
				P->bFireHeld = false;
				const bool bPass = T->M->Health < T->Health0 - 100;
				UGameplayStatics::ApplyDamage(T->M.Get(), 100000.f, nullptr, nullptr, nullptr);
				return bPass ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 9a. monsters waiting for the player sleep (no actor or movement tick) and CLASSIC ones have
	// no AI controller; one woken by a shot ticks again ---------------------------------------------
	Run->Steps.Add({ TEXT("sleepers"),
		[W](FString& Msg)
		{
			return true;
		},
		[W, Player](int32 Tics, FString& Msg)
		{
			int32 Idle = 0, Asleep = 0, Controllers = 0;
			ADoomMonster* Sleeper = nullptr;
			for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
			{
				if (!M.IsValid() || M->IsDead()) continue;
				Controllers += M->GetController() ? 1 : 0;
				if (M->State != EMonsterState::Idle) continue;
				++Idle;
				if (M->IsIdleAsleep() && !M->IsActorTickEnabled() && !M->GetCharacterMovement()->IsComponentTickEnabled())
				{
					++Asleep;
					if (!Sleeper) Sleeper = M.Get();
				}
			}
			bool bWoke = false;
			if (Sleeper)
			{
				UGameplayStatics::ApplyDamage(Sleeper, 1.f, nullptr, Player(), nullptr);
				bWoke = !Sleeper->IsIdleAsleep() && Sleeper->IsActorTickEnabled() && Sleeper->GetCharacterMovement()->IsComponentTickEnabled();
			}
			Msg = FString::Printf(TEXT("%d of %d monsters waiting for the player are asleep; %d AI controllers (CLASSIC: none); a shot one woke and ticks: %d"),
				Asleep, Idle, Controllers, bWoke);
			const bool bClassic = !W->IsSmartAI();
			return Idle > 0 && Asleep * 10 >= Idle * 8 && (!bClassic || Controllers == 0) && bWoke ? EStep::Pass : EStep::Fail;
		} });

	// --- 9. awake monsters actually walk (no freezing in low rooms or doorways) -------------------
	{
		struct FChase { TMap<TWeakObjectPtr<ADoomMonster>, FVector2f> Start, Last; TMap<TWeakObjectPtr<ADoomMonster>, float> Path; };
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
						C->Last.Add(M, M->DoomPos());
						C->Path.Add(M, 0.f);
					}
				}
				if (C->Start.Num() == 0) { Msg = TEXT("no monsters"); return false; }
				return true;
			},
			[C](int32 Tics, FString& Msg)
			{
				// how far each one walked, tic by tic: a monster pacing inside a closed room still walks
				for (TPair<TWeakObjectPtr<ADoomMonster>, FVector2f>& E : C->Last)
				{
					if (!E.Key.IsValid()) continue;
					C->Path[E.Key] += FVector2f::Distance(E.Key->DoomPos(), E.Value);
					E.Value = E.Key->DoomPos();
				}
				if (Tics < 175) return EStep::Running;
				int32 Moved = 0, Alive = 0;
				FString Stuck;
				for (const TPair<TWeakObjectPtr<ADoomMonster>, FVector2f>& E : C->Start)
				{
					if (!E.Key.IsValid() || E.Key->IsDead()) continue;
					++Alive;
					if (FVector2f::Distance(E.Key->DoomPos(), E.Value) > 32.f || C->Path[E.Key] > 64.f) ++Moved;
					else
					{
						// why: free walking directions, the room's height, and whether the sector is closed in
						int32 Dirs = 0;
						for (int32 d = 0; d < 8; ++d) Dirs += E.Key->TestMove(d);
						const UDoomWorld* DW = E.Key->DoomWorld;
						const int32 Sec = DW->GetMap()->SectorAt(E.Key->DoomPos());
						const FDoomSector* S = Sec >= 0 ? &DW->GetMap()->Sectors[Sec] : nullptr;
						Stuck += FString::Printf(TEXT(" %s(%.0f,%.0f dirs %d room %.0f walked %.0f)"), *E.Key->Def->Name, E.Value.X, E.Value.Y, Dirs,
							S ? S->Ceiling - S->Floor : -1.f, C->Path[E.Key]);
					}
				}
				Msg = FString::Printf(TEXT("%d of %d awake monsters got 32 units away or walked 64 in 5 s; still:%s"), Moved, Alive, *Stuck);
				return Alive > 0 && Moved * 10 >= Alive * 9 ? EStep::Pass : EStep::Fail;
			} });
	}


	// --- 9b. SMART monsters: AI controllers run the Behavior Tree, perception senses the player, and
	// the ones that can walk to the player close in along FDoomNav paths or get it in sight ----------
	{
		struct FSmart
		{
			TMap<TWeakObjectPtr<ADoomMonster>, float> StartWalk; // path length to the player at the start
			TSet<TWeakObjectPtr<ADoomMonster>> Saw;
			bool bWasSmart = false;
		};
		TSharedRef<FSmart> C = MakeShared<FSmart>();
		auto WalkTo = [](const ADoomMonster* M, const FVector2f& To)
		{
			TArray<FVector2f> Path;
			const UDoomWorld* DW = M->GetDoomWorld();
			if (!DW->GetNav().FindPath(*DW->GetMap(), M->DoomPos(), To, M->Def->Radius, M->Def->Height, (M->Def->Flags & EThingFlag::Float) != 0, Path)) return -1.f;
			float Len = 0;
			FVector2f Prev = M->DoomPos();
			for (const FVector2f& P : Path) { Len += FVector2f::Distance(Prev, P); Prev = P; }
			return Len;
		};
		Run->Steps.Add({ TEXT("smartai"),
			[W, C, GoHome, Player, WalkTo](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				P->bGodMode = true;
				C->bWasSmart = W->IsSmartAI();
				// -smartbaseline: the same measure with CLASSIC monsters, to compare
				W->SetSmartAI(!FParse::Param(FCommandLine::Get(), TEXT("smartbaseline")));
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
				{
					if (!M.IsValid() || M->IsDead() || M->Def->Speed <= 0) continue;
					M->Target = P;
					M->StartState(EMonsterState::Chase);
					const float Walk = WalkTo(M.Get(), P->DoomPos());
					if (Walk >= 0) C->StartWalk.Add(M, Walk);
				}
				if (C->StartWalk.Num() == 0) { Msg = TEXT("no monster can walk to the player"); return false; }
				return true;
			},
			[W, C, Player, WalkTo](int32 Tics, FString& Msg)
			{
				ADoomPlayer* P = Player();
				for (const TPair<TWeakObjectPtr<ADoomMonster>, float>& E : C->StartWalk)
				{
					if (E.Key.IsValid() && !E.Key->IsDead() && W->CheckSight(E.Key.Get(), P)) C->Saw.Add(E.Key);
				}
				if (Tics < 350) return EStep::Running;
				int32 Controllers = 0, Running = 0, Sensed = 0, Alive = 0, Engaged = 0, Closer = 0;
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
				{
					if (!M.IsValid() || M->IsDead()) continue;
					const ADoomAIController* AI = Cast<ADoomAIController>(M->GetController());
					Controllers += AI ? 1 : 0;
					Running += AI && AI->GetBrainComponent() && AI->GetBrainComponent()->IsRunning() ? 1 : 0;
					Sensed += AI ? AI->NumSensed : 0;
				}
				for (const TPair<TWeakObjectPtr<ADoomMonster>, float>& E : C->StartWalk)
				{
					if (!E.Key.IsValid() || E.Key->IsDead()) continue;
					++Alive;
					const float Now = WalkTo(E.Key.Get(), P->DoomPos());
					const bool bCloser = Now >= 0 && Now < E.Value - 64.f;
					Closer += bCloser ? 1 : 0;
					Engaged += (bCloser || C->Saw.Contains(E.Key) || E.Value < 128.f) ? 1 : 0;
				}
				int32 Live = 0;
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters()) Live += M.IsValid() && !M->IsDead() ? 1 : 0;
				Msg = FString::Printf(TEXT("%d of %d live monsters have the AI controller (%d trees running), %d perception events; of %d that can walk to the player, %d closed in by 64+ units and %d closed in or saw the player in 10 s"),
					Controllers, Live, Running, Sensed, Alive, Closer, Engaged);
				const bool bBaseline = !W->IsSmartAI();
				if (!C->bWasSmart) W->SetSmartAI(false);
				if (bBaseline) { Msg = TEXT("CLASSIC baseline: ") + Msg; return EStep::Pass; }
				const bool bOk = Controllers == Live && Running == Live && Sensed > 0 && Alive > 0 && Engaged * 10 >= Alive * 7;
				return bOk ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 10. the automap learns the walls in view while it is closed -----------------------------
	Run->Steps.Add({ TEXT("automap"),
		[W, Map, GoHome](FString& Msg)
		{
			GoHome();
			W->bAutomap = false;
			for (FDoomLine& L : Map->Lines) L.bSeen = false;
			return true;
		},
		[W, Map, Player, Shot](int32 Tics, FString& Msg)
		{
			if (Tics < 20) return EStep::Running;
			// then a look at the map itself: open, zoomed out, with the grid and a mark (playtest_automap.png)
			ADoomHUD* HUD = Cast<ADoomHUD>(Cast<APlayerController>(Player()->GetController())->GetHUD());
			if (Tics == 20 && HUD)
			{
				W->bAutomap = true;
				HUD->AutomapZoom(-1.f, 1.f);
				HUD->AutomapToggleGrid();
				HUD->AutomapMark();
			}
			if (Tics == 24) Shot(TEXT("automap"));
			if (Tics < 27) return EStep::Running;
			if (HUD)
			{
				HUD->AutomapToggleGrid();
				HUD->AutomapClearMarks();
				HUD->AutomapZoom(1.f, 1.f);
			}
			W->bAutomap = false;
			const int32 Here = Map->SectorAt(Player()->DoomPos());
			int32 Seen = 0, Away = 0;
			for (int32 i = 0; i < Map->Lines.Num(); ++i)
			{
				const FDoomLine& L = Map->Lines[i];
				if (!L.bSeen) continue;
				++Seen;
				if (L.FrontSector != Here && L.BackSector != Here) ++Away;
			}
			Msg = FString::Printf(TEXT("%d lines known after 20 tics with the map closed, %d of them away from the player's sector"), Seen, Away);
			// the old code learned nothing with the map closed; a generated start room has only ~7 lines
			return Seen >= 5 && Away > 0 ? EStep::Pass : EStep::Fail;
		} });

	// --- 11. a stray click on the quit prompt cancels it (only Y, Enter or A confirm) ------------
	Run->Steps.Add({ TEXT("quitprompt"),
		[W, Player](FString& Msg)
		{
			W->Menu.Open(*W, EMenuPage::Quit);
			APlayerController* PC = Cast<APlayerController>(Player()->GetController());
			if (W->Menu.Prompt().IsEmpty() || !PC) { Msg = TEXT("the quit page has no prompt"); return false; }
			W->bInputLocked = false;
			PC->InputKey(FInputKeyEventArgs(nullptr, FInputDeviceId::CreateFromInternalId(0), EKeys::LeftMouseButton, IE_Pressed, FPlatformTime::Cycles64()));
			PC->InputKey(FInputKeyEventArgs(nullptr, FInputDeviceId::CreateFromInternalId(0), EKeys::LeftMouseButton, IE_Released, FPlatformTime::Cycles64()));
			W->bInputLocked = true;
			return true;
		},
		[W](int32 Tics, FString& Msg)
		{
			if (Tics < 3) return EStep::Running; // a quit would have ended the run by now
			const bool bClosedPrompt = W->GetState() != EDoomGameState::Menu || W->Menu.Prompt().IsEmpty();
			Msg = FString::Printf(TEXT("after a left click: still running, state %d, prompt \"%s\""), (int32)W->GetState(), *W->Menu.Prompt().Left(20));
			W->Menu.Close(*W);
			return bClosedPrompt ? EStep::Pass : EStep::Fail;
		} });

	// --- 12. switching the renderer in the middle of a level keeps the game where it was ----------
	{
		struct FSwap { FVector2f Pos; int32 Kills = 0, Dead = 0, Tic = 0; bool bModern = false; };
		TSharedRef<FSwap> X = MakeShared<FSwap>();
		Run->Steps.Add({ TEXT("renderer"),
			[W, X, GoHome, Player](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				// kill one monster first, so a reload from scratch would show
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
				{
					if (M.IsValid() && !M->IsDead()) { UGameplayStatics::ApplyDamage(M.Get(), 100000.f, P->GetController(), P, nullptr); break; }
				}
				X->Pos = P->DoomPos();
				X->Kills = W->Flow().Kills;
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters()) X->Dead += M.IsValid() && M->IsDead();
				X->Tic = W->GetTic();
				X->bModern = W->IsModern();
				W->Menu.Open(*W, EMenuPage::Options);
				W->SetModern(!X->bModern);
				return true;
			},
			[W, X, Shot](int32 Tics, FString& Msg)
			{
				// the Options page itself (playtest_options.png): every row fits above the status bar
				if (Tics < 10) W->Menu.Item = 10; // CONTROLS, the last row: the page scrolls to it
				if (Tics == 10) Shot(TEXT("options"));
				if (Tics < 14) return EStep::Running;
				ADoomPlayer* P = W->GetPlayer();
				int32 Dead = 0;
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters()) Dead += M.IsValid() && M->IsDead();
				Msg = FString::Printf(TEXT("modern %d -> %d, state %d (menu %d), player (%.0f,%.0f) -> (%.0f,%.0f), kills %d -> %d, corpses %d -> %d, tic %d -> %d"),
					X->bModern, W->IsModern(), (int32)W->GetState(), (int32)EDoomGameState::Menu, X->Pos.X, X->Pos.Y, P ? P->DoomPos().X : 0, P ? P->DoomPos().Y : 0,
					X->Kills, W->Flow().Kills, X->Dead, Dead, X->Tic, W->GetTic());
				const bool bPass = P && W->IsModern() != X->bModern && W->GetState() == EDoomGameState::Menu && FVector2f::Distance(P->DoomPos(), X->Pos) < 1.f
					&& W->Flow().Kills == X->Kills && Dead == X->Dead;
				W->Menu.Close(*W);
				W->SetModern(X->bModern);
				return bPass ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 13. P_GiveAmmo's weapon switches: shells take the pistol to the shotgun, bullets don't take it
	// to the chaingun, rockets don't take it to the launcher -----------------------------------------
	TSharedRef<FString> AmmoResult = MakeShared<FString>();
	Run->Steps.Add({ TEXT("ammoswitch"),
		[W, GoHome, Player, AmmoResult](FString& Msg)
		{
			GoHome();
			ADoomPlayer* P = Player();
			const FDoomThingDef* Shells = DoomInfo::FindThing(2008);
			const FDoomThingDef* Clip = DoomInfo::FindThing(2007);
			const FDoomThingDef* Rocket = DoomInfo::FindThing(2010);
			if (!Shells || !Clip || !Rocket) { Msg = TEXT("no ammo things in the tables"); return false; }
			FString Got;
			auto Try = [&](const FDoomThingDef* Def, EDoomAmmo A, EDoomWeapon Expect, const TCHAR* Label)
			{
				P->Inv().Ready = EDoomWeapon::Pistol;
				P->PendingWeapon = EDoomWeapon::Count;
				for (bool& B : P->Inv().Weapons) B = true;
				P->Inv().Ammo[(int32)A] = 0;
				P->TryPickup(*Def, false);
				const bool bOk = P->PendingWeapon == Expect;
				Got += FString::Printf(TEXT("%s -> %s%s; "), Label, P->PendingWeapon == EDoomWeapon::Count ? TEXT("stay") : *UEnum::GetDisplayValueAsText(P->PendingWeapon).ToString(), bOk ? TEXT("") : TEXT(" (wrong)"));
				return bOk;
			};
			bool bAll = Try(Shells, EDoomAmmo::Shells, EDoomWeapon::Shotgun, TEXT("shells"));
			bAll &= Try(Clip, EDoomAmmo::Bullets, EDoomWeapon::Count, TEXT("clip"));
			bAll &= Try(Rocket, EDoomAmmo::Rockets, EDoomWeapon::Count, TEXT("rocket"));
			P->PendingWeapon = EDoomWeapon::Count;
			*AmmoResult = Got + (bAll ? TEXT("as vanilla") : TEXT("not vanilla"));
			return true;
		},
		[AmmoResult](int32 Tics, FString& Msg)
		{
			Msg = *AmmoResult;
			return AmmoResult->Contains(TEXT("not vanilla")) ? EStep::Fail : EStep::Pass;
		} });

	// --- 14. a lost soul plays its death frames before it vanishes --------------------------------
	{
		struct FSoul { TWeakObjectPtr<ADoomMonster> M; bool bSeenDying = false; };
		TSharedRef<FSoul> L = MakeShared<FSoul>();
		Run->Steps.Add({ TEXT("lostsoul"),
			[W, L, GoHome, Player, ClearestYaw](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				float Clear = 0;
				const float Yaw = ClearestYaw(P, Clear);
				if (Clear < 120.f) { Msg = TEXT("no room at the start"); return false; }
				const FVector Feet = P->GetActorLocation() - FVector(0, 0, Doom::HalfHeight(P)) + FRotator(0, Yaw, 0).Vector() * 96.f * Doom::Scale;
				L->M = W->SpawnMonster(3006, Feet, Doom::YawToAngle(Yaw) + 180.f);
				if (!L->M.IsValid()) { Msg = TEXT("could not spawn a lost soul"); return false; }
				UGameplayStatics::ApplyDamage(L->M.Get(), 1000.f, P->GetController(), P, nullptr);
				return true;
			},
			[L](int32 Tics, FString& Msg)
			{
				if (L->M.IsValid() && L->M->State == EMonsterState::Dying) L->bSeenDying = true;
				Msg = FString::Printf(TEXT("dying frames seen %d, gone after %d tics %d"), L->bSeenDying, Tics, !L->M.IsValid());
				if (L->M.IsValid() && Tics < 105) return EStep::Running;
				return L->bSeenDying && !L->M.IsValid() ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 15. a corpse resting on a lift goes down with it (a sleeping corpse wakes when its floor drops)
	{
		struct FLiftCorpse { int32 Line = -1, Sector = -1, Special = 0; TWeakObjectPtr<ADoomMonster> M; bool bAsleep = false; float Low = 1e9f; float Gap = 1e9f; };
		TSharedRef<FLiftCorpse> C = MakeShared<FLiftCorpse>();
		// picked now: the lift step may use up a W1 lift line, and the renderer step reloads the map (so
		// the step below looks the map up again rather than keep a pointer)
		C->Line = FindLine({ 62, 88, 10, 21, 120, 121, 122, 123 }, false, true);
		if (C->Line >= 0)
		{
			C->Special = Map->Lines[C->Line].Special;
			C->Sector = Map->SectorsWithTag(Map->Lines[C->Line].Tag)[0];
		}
		Run->Steps.Add({ TEXT("liftcorpse"),
			[W, C, Player](FString& Msg)
			{
				if (C->Line < 0) { Msg = TEXT("no lift line"); return false; }
				FDoomMap* Map = W->GetMap();
				Map->Lines[C->Line].Special = C->Special; // the lift step may have used it up
				const FDoomSector& S = Map->Sectors[C->Sector];
				if (W->SectorHasMover(C->Sector)) { Msg = TEXT("the lift is busy"); return false; }
				C->M = W->SpawnMonster(3004, Doom::ToUE(S.Center, S.Floor), 0.f); // a zombieman on the lift
				if (!C->M.IsValid()) { Msg = TEXT("could not spawn on the lift"); return false; }
				UGameplayStatics::ApplyDamage(C->M.Get(), 1000.f, nullptr, nullptr, nullptr);
				return true;
			},
			[W, C, Player](int32 Tics, FString& Msg)
			{
				if (!C->M.IsValid()) { Msg = TEXT("the corpse vanished"); return EStep::Fail; }
				const FDoomSector& S = W->GetMap()->Sectors[C->Sector];
				if (!C->bAsleep)
				{
					if (C->M->IsCorpseAsleep())
					{
						C->bAsleep = true;
						W->ActivateSpecial(C->Line, 0, Player(), EActivation::Use, true); // down it goes
					}
					return Tics > 200 ? EStep::Fail : EStep::Running;
				}
				// while the lift waits at the bottom the corpse must come to lie on the floor, not float
				// where the lift was (the gap at the bottom: its smallest value once it has fallen)
				const float Feet = C->M->DoomZ();
				if (W->SectorHasMover(C->Sector))
				{
					if (S.Floor < C->Low - 0.01f) { C->Low = S.Floor; C->Gap = Feet - S.Floor; }
					else if (FMath::IsNearlyEqual(S.Floor, C->Low, 0.01f)) { C->Gap = FMath::Min(C->Gap, Feet - S.Floor); }
				}
				Msg = FString::Printf(TEXT("sector %d: corpse asleep %d, lowest floor %.0f, corpse settled %.1f units above it, floor now %.0f, corpse %.0f"),
					C->Sector, C->bAsleep, C->Low, C->Gap, S.Floor, Feet);
				if (W->SectorHasMover(C->Sector) && Tics < 400) return EStep::Running;
				const bool bPass = C->Gap < 2.f && FMath::Abs(Feet - S.Floor) < 2.f;
				C->M->Destroy();
				return bPass ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 16. corpses go to sleep (no movement, actor or sprite ticks) and still face the camera ----
	{
		struct FCorpses { int32 Dead = 0; TWeakObjectPtr<ADoomMonster> Near; float Yaw0 = 0; bool bMoved = false; int32 MovedTic = 0; };
		TSharedRef<FCorpses> K = MakeShared<FCorpses>();
		Run->Steps.Add({ TEXT("corpses"),
			[W, K, GoHome, Player](FString& Msg)
			{
				GoHome();
				ADoomPlayer* P = Player();
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
				{
					if (M.IsValid() && !M->IsDead()) UGameplayStatics::ApplyDamage(M.Get(), 100000.f, P->GetController(), P, nullptr);
				}
				return true;
			},
			[W, K, Player](int32 Tics, FString& Msg)
			{
				ADoomPlayer* P = Player();
				int32 Dead = 0, MoveTicks = 0, ActorTicks = 0, SpriteTicks = 0;
				for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
				{
					if (!M.IsValid() || !M->IsDead()) continue;
					++Dead;
					MoveTicks += M->GetCharacterMovement()->IsComponentTickEnabled();
					ActorTicks += M->IsActorTickEnabled();
					SpriteTicks += M->Sprite->IsComponentTickEnabled();
				}
				if (Tics < 140) return EStep::Running; // death frames, falls and landings
				if (!K->bMoved)
				{
					// the nearest corpse in view: step sideways and watch it turn to the new camera spot
					float Best = 1e9f;
					for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters())
					{
						if (!M.IsValid() || !M->IsDead() || !M->Sprite->IsSleeping() || !M->Sprite->WasRecentlyRendered(0.2f)) continue;
						const float D = FVector::Dist(M->GetActorLocation(), P->GetActorLocation());
						if (D < Best) { Best = D; K->Near = M; }
					}
					K->Dead = Dead;
					if (K->Near.IsValid())
					{
						K->Yaw0 = K->Near->Sprite->GetComponentRotation().Yaw;
						const FVector Side = FRotationMatrix(FRotator(0, (K->Near->GetActorLocation() - P->GetActorLocation()).Rotation().Yaw, 0)).GetUnitAxis(EAxis::Y);
						P->SetActorLocation(P->GetActorLocation() + Side * 48.f * Doom::Scale, true);
					}
					K->bMoved = true;
					K->MovedTic = Tics;
					Msg = FString::Printf(TEXT("%d corpses: movement ticks %d, actor ticks %d, sprite ticks %d, batched sprites %d"),
						Dead, MoveTicks, ActorTicks, SpriteTicks, W->NumSleepingSprites());
					return EStep::Running;
				}
				if (Tics - K->MovedTic < 4) return EStep::Running;
				FString Facing = TEXT("no corpse in view to check facing");
				bool bFacing = true;
				if (K->Near.IsValid())
				{
					const float Yaw1 = K->Near->Sprite->GetComponentRotation().Yaw;
					const float Want = (K->Near->GetActorLocation() - P->EyeLocation()).Rotation().Yaw;
					bFacing = FMath::Abs(FMath::FindDeltaAngleDegrees(Yaw1, Want)) < 3.f && FMath::Abs(FMath::FindDeltaAngleDegrees(Yaw1, K->Yaw0)) > 1.f;
					Facing = FString::Printf(TEXT("a sleeping corpse turned %.1f degrees to face the moved camera (off by %.1f)"),
						FMath::FindDeltaAngleDegrees(K->Yaw0, Yaw1), FMath::FindDeltaAngleDegrees(Yaw1, Want));
				}
				Msg = FString::Printf(TEXT("%d corpses: movement ticks %d, actor ticks %d, sprite ticks %d, batched sprites %d; %s"),
					Dead, MoveTicks, ActorTicks, SpriteTicks, W->NumSleepingSprites(), *Facing);
				// a few may still be settling (a corpse that fell off a ledge); nearly all must sleep
				const bool bSleeping = Dead > 0 && MoveTicks * 10 <= Dead && SpriteTicks * 10 <= Dead;
				return bSleeping && bFacing ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 17. one-frame things (decorations, items) sleep; animated ones (torches, glowing items) tick
	Run->Steps.Add({ TEXT("things"),
		[](FString& Msg) { return true; },
		[W](int32 Tics, FString& Msg)
		{
			if (Tics < 5) return EStep::Running;
			int32 Still = 0, StillAsleep = 0, Animated = 0, AnimatedTicking = 0;
			for (const TWeakObjectPtr<ADoomThing>& T : W->GetThings())
			{
				if (!T.IsValid() || T->bEffect || T->bDead || !T->Sprite) continue;
				if (T->Sprite->CurrentSequence().Len() == 1)
				{
					++Still;
					StillAsleep += T->Sprite->IsSleeping() && !T->Sprite->IsComponentTickEnabled();
				}
				else
				{
					++Animated;
					AnimatedTicking += T->Sprite->IsComponentTickEnabled();
				}
			}
			Msg = FString::Printf(TEXT("%d one-frame things, %d asleep; %d animated, %d still ticking; %d sprites in the batched pass (sprite ticks before: %d)"),
				Still, StillAsleep, Animated, AnimatedTicking, W->NumSleepingSprites(), Still + Animated);
			return Still > 0 && StillAsleep == Still && AnimatedTicking == Animated ? EStep::Pass : EStep::Fail;
		} });

	// --- 18. rebinding: the Controls list, Fire moved to F and back to the defaults ----------------
	{
		struct FRebind { int32 Phase = 0; int32 PhaseTic = 0; FString Report; bool bOk = true; };
		TSharedRef<FRebind> B = MakeShared<FRebind>();
		Run->Steps.Add({ TEXT("rebind"),
			[](FString& Msg) { return true; },
			[W, B, Player, Shot](int32 Tics, FString& Msg)
			{
				ADoomPlayerController* PC = Cast<ADoomPlayerController>(Player()->GetController());
				UEnhancedInputLocalPlayerSubsystem* Sub = PC && PC->GetLocalPlayer() ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()) : nullptr;
				if (!PC || !Sub) { Msg = TEXT("no Doom controller or Enhanced Input subsystem"); return EStep::Fail; }
				auto FireKeys = [&]()
				{
					TArray<FString> Names;
					for (const FKey& K : Sub->QueryKeysMappedToAction(PC->FireAction)) Names.Add(K.ToString());
					return FString::Join(Names, TEXT(" "));
				};
				auto Has = [&](const FKey& K) { return Sub->QueryKeysMappedToAction(PC->FireAction).Contains(K); };
				if (Tics - B->PhaseTic < 3) return EStep::Running; // the subsystem rebuilds its mappings next frame
				switch (B->Phase)
				{
				case 0:
				{
					const TArray<ADoomPlayerController::FBinding> Bindings = PC->GetBindings();
					const ADoomPlayerController::FBinding* FireRow = Bindings.FindByPredicate([](const ADoomPlayerController::FBinding& X) { return X.Name == TEXT("Fire"); });
					B->bOk &= Bindings.Num() == DoomInput::Mappables().Num() && FireRow && FireRow->Keys.Num() == 2;
					B->Report = FString::Printf(TEXT("%d controls listed, Fire on [%s] (keys in play: %s)"), Bindings.Num(),
						FireRow ? *FString::JoinBy(FireRow->Keys, TEXT(" "), [](const FKey& K) { return K.ToString(); }) : TEXT("-"), *FireKeys());
					B->bOk &= Has(EKeys::LeftMouseButton) && Has(EKeys::LeftControl);
					B->bOk &= PC->Rebind(TEXT("Fire"), EKeys::F);
					break;
				}
				case 1:
				{
					B->Report += FString::Printf(TEXT("; after binding F: %s"), *FireKeys());
					B->bOk &= Has(EKeys::F) && !Has(EKeys::LeftMouseButton) && Has(EKeys::LeftControl);
					// the Controls page as the player sees it, on the Fire row
					W->Menu.Open(*W, EMenuPage::Options);
					W->Menu.Item = 10; // CONTROLS
					W->Menu.Select(*W);
					W->Menu.Item = DoomInput::Mappables().IndexOfByPredicate([](const DoomInput::FMappable& M) { return M.Name == TEXT("Fire"); });
					const TArray<FMenuRow> Rows = W->Menu.Rows(*W);
					B->bOk &= W->Menu.Page == EMenuPage::Controls && Rows.IsValidIndex(W->Menu.Item) && Rows[W->Menu.Item].ValueText.StartsWith(TEXT("F,"));
					B->Report += FString::Printf(TEXT("; the menu row reads \"%s  %s\""), Rows.IsValidIndex(W->Menu.Item) ? *Rows[W->Menu.Item].Text : TEXT("?"),
						Rows.IsValidIndex(W->Menu.Item) ? *Rows[W->Menu.Item].ValueText : TEXT("?"));
					Shot(TEXT("controls"));
					// the gamepad's fire button, from the right trigger to the left
					B->bOk &= PC->Rebind(TEXT("PadFire"), EKeys::Gamepad_LeftTrigger);
					break;
				}
				case 2:
				{
					B->Report += FString::Printf(TEXT("; PAD FIRE on LT: %s"), *FireKeys());
					B->bOk &= Has(EKeys::Gamepad_LeftTrigger) && !Has(EKeys::Gamepad_RightTrigger) && Has(EKeys::F);
					W->Menu.Close(*W);
					PC->ResetBindings();
					break;
				}
				case 3:
					B->Report += FString::Printf(TEXT("; after reset: %s"), *FireKeys());
					B->bOk &= Has(EKeys::LeftMouseButton) && Has(EKeys::LeftControl) && !Has(EKeys::F)
						&& Has(EKeys::Gamepad_RightTrigger) && !Has(EKeys::Gamepad_LeftTrigger);
					Msg = B->Report;
					return B->bOk ? EStep::Pass : EStep::Fail;
				}
				++B->Phase;
				B->PhaseTic = Tics;
				Msg = B->Report;
				return EStep::Running;
			} });
	}

	// --- 18b. player colours: the marine's frames for players 2-4 trade the green ramp for grey, brown
	// and red (Doom's translation tables) --------------------------------------------------------------
	Run->Steps.Add({ TEXT("colours"),
		[](FString& Msg) { return true; },
		[W](int32 Tics, FString& Msg)
		{
			auto Count = [](const FDoomPicture* P, int32 Lo)
			{
				int32 N = 0;
				if (P) for (int16 I : P->Indices) N += (I >= Lo && I <= Lo + 15) ? 1 : 0;
				return N;
			};
			const FDoomPicture* Green = W->GetGraphics()->SpriteFrame(TEXT("PLAY"), 'A', 1, 0).Picture;
			const int32 GreenPixels = Count(Green, 0x70);
			bool bOk = GreenPixels > 0;
			static const int32 Bases[4] = { 0x70, 0x60, 0x40, 0x20 };
			FString Report = FString::Printf(TEXT("PLAYA1: %d green pixels"), GreenPixels);
			for (int32 t = 1; t <= 3; ++t)
			{
				const FDoomPicture* P = W->GetGraphics()->SpriteFrame(TEXT("PLAY"), 'A', 1, t).Picture;
				const int32 Left = Count(P, 0x70), Moved = Count(P, Bases[t]) - Count(Green, Bases[t]);
				bOk &= P && P != Green && Left == 0 && Moved == GreenPixels;
				Report += FString::Printf(TEXT("; player %d: %d green left, %d moved to 0x%02X"), t + 1, Left, Moved, Bases[t]);
			}
			Msg = Report;
			return bOk ? EStep::Pass : EStep::Fail;
		} });

	// --- 18c. -fast: demons step twice as often (G_InitNew halves their frames); imp balls fly at 20 ---
	Run->Steps.Add({ TEXT("fast"),
		[](FString& Msg) { return true; },
		[W, Player](int32 Tics, FString& Msg)
		{
			const bool bWas = W->bFastParam;
			W->bFastParam = false;
			ADoomPlayer* P = Player();
			const FVector At = P->GetActorLocation() + P->GetActorForwardVector() * 96.f * Doom::Scale;
			ADoomMonster* Slow = W->SpawnMonster(3002, At, 0);
			const int32 SlowTics = Slow ? Slow->RunTics() : -1; // read before -fast goes on
			W->bFastParam = true;
			ADoomMonster* Fast = W->SpawnMonster(3002, At + FVector(0, 0, 80.f * Doom::Scale), 0);
			const bool bOk = Slow && Fast && Fast->RunTics() * 2 == SlowTics
				&& FMath::IsNearlyEqual(Fast->GetCharacterMovement()->MaxWalkSpeed, Slow->GetCharacterMovement()->MaxWalkSpeed * 2.f, 1.f) && W->IsFast();
			Msg = FString::Printf(TEXT("demon run frames %d -> %d, speed %.0f -> %.0f"), SlowTics, Fast ? Fast->RunTics() : -1,
				Slow ? Slow->GetCharacterMovement()->MaxWalkSpeed : 0.f, Fast ? Fast->GetCharacterMovement()->MaxWalkSpeed : 0.f);
			if (Slow) Slow->Destroy();
			if (Fast) Fast->Destroy();
			W->bFastParam = bWas;
			return bOk ? EStep::Pass : EStep::Fail;
		} });

	// --- 18d. -altdeath (deathmatch 2.0): an item picked up comes back where it was 30 s later --------
	{
		struct FAlt { int32 Type = 0; FVector At = FVector::ZeroVector; bool bWasDM = false, bWasAlt = false; };
		TSharedRef<FAlt> A = MakeShared<FAlt>();
		Run->Steps.Add({ TEXT("altdeath"),
			[W, A, Player](FString& Msg)
			{
				A->bWasDM = W->Flow().bDeathmatch;
				A->bWasAlt = W->bAltDeathParam;
				W->Flow().bDeathmatch = true;
				W->bAltDeathParam = true;
				ADoomPlayer* P = Player();
				P->Inv().Ammo[(int32)EDoomAmmo::Bullets] = 0;
				for (const TWeakObjectPtr<ADoomThing>& T : W->GetThings())
				{
					if (!T.IsValid() || T->bEffect || T->bPooled || T->bDropped || !T->Def || T->Def->DoomEdNum != 2007) continue; // a clip
					A->Type = 2007;
					A->At = T->GetActorLocation();
					T->OnOverlap(nullptr, P, nullptr, 0, false, FHitResult());
					break;
				}
				if (!A->Type) { Msg = TEXT("no clip on the map"); W->Flow().bDeathmatch = A->bWasDM; W->bAltDeathParam = A->bWasAlt; return false; }
				return true;
			},
			[W, A](int32 Tics, FString& Msg)
			{
				auto ClipThere = [&]()
				{
					for (const TWeakObjectPtr<ADoomThing>& T : W->GetThings())
					{
						if (T.IsValid() && !T->bPooled && T->Def && T->Def->DoomEdNum == A->Type && FVector::Dist2D(T->GetActorLocation(), A->At) < 1.f) return true;
					}
					return false;
				};
				if (Tics == 5 && ClipThere()) { Msg = TEXT("the clip was not taken"); W->Flow().bDeathmatch = A->bWasDM; W->bAltDeathParam = A->bWasAlt; return EStep::Fail; }
				if (Tics < 30 * 35 + 5) return EStep::Running;
				const bool bBack = ClipThere();
				Msg = FString::Printf(TEXT("a clip taken at (%.0f,%.0f): back after 30 s: %d"), Doom::ToDoom2D(A->At).X, Doom::ToDoom2D(A->At).Y, bBack);
				W->Flow().bDeathmatch = A->bWasDM;
				W->bAltDeathParam = A->bWasAlt;
				return bBack ? EStep::Pass : EStep::Fail;
			} });
	}

	// --- 19 (last: it leaves the map). the intermission shows the vanilla par time ----------------
	Run->Steps.Add({ TEXT("intermission"),
		[this, W](FString& Msg)
		{
			ExitLevel(false);
			return W->GetState() == EDoomGameState::Intermission;
		},
		[W, Shot](int32 Tics, FString& Msg)
		{
			const int32 Par = DoomInfo::ParTime(W->GetMapName());
			Msg = FString::Printf(TEXT("%s par %d s (vanilla E1M1 30, MAP01 30), intermission tic %d"), *W->GetMapName(), Par, W->Flow().IntermissionTic);
			if (Tics == 170) Shot(TEXT("intermission"));
			if (Tics < 175) return EStep::Running;
			// a generated map has no par time, and the intermission leaves it out
			uint32 Seed = 0;
			if (DoomMapGen::ParseName(W->GetMapName(), Seed)) return Par < 0 ? EStep::Pass : EStep::Fail;
			return Par > 0 ? EStep::Pass : EStep::Fail;
		} });

	// run the steps one after the other, one poll per Doom tic
	TSharedRef<FPlayRun> R = Run;
	GetWorldTimerManager().SetTimer(Run->Timer, [this, W, R, Home, HomeView, GoHome]()
	{
		ADoomPlayer* P = W->GetPlayer();
		const bool bOffPlay = R->Index >= 0 && R->Index < R->Steps.Num() && (R->Steps[R->Index].Name == TEXT("quitprompt") || R->Steps[R->Index].Name == TEXT("rebind")
			|| R->Steps[R->Index].Name == TEXT("renderer") || R->Steps[R->Index].Name == TEXT("intermission"));
		if (!P || (W->GetState() != EDoomGameState::Playing && !bOffPlay))
		{
			return;
		}
		if (R->bHoldForward)
		{
			P->AddMovementInput(FRotator(0, P->GetControlRotation().Yaw, 0).Vector(), 1.f);
		}
		// Doom's tics stop in the menu and the intermission: count 1/35 s of real time there instead
		const int32 Now = W->GetState() == EDoomGameState::Playing && !bOffPlay ? W->GetTic() : (int32)(W->GetWorld()->GetRealTimeSeconds() * Doom::TicRate);
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

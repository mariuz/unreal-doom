// UDoomWorld: line activation (use, walk-over, shoot), switches, locks, teleporters, the noise
// alert and specials run by tag. See DoomWorldSpecialTable.h for the special table.
#include "DoomWorld.h"
#include "DoomWorldSpecialTable.h"
#include "Engine/DamageEvents.h"
#include "Misc/ScopeExit.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "DoomUE.h"
#include "DoomWad.h"
#include "DoomGraphics.h"
#include "DoomSound.h"
#include "DoomLevel.h"
#include "DoomMapTiles.h"
#include "DoomThing.h"
#include "DoomMonster.h"
#include "DoomProjectile.h"
#include "DoomPlayer.h"
#include "DoomSettings.h"
#include "DoomAssets.h"
#include "DoomMapActors.h"
#include "DoomGameMode.h"
#include "EngineUtils.h"
#include "DoomGameState.h"
#include "DoomPlayerState.h"
#include "DoomMusic.h"
#include "DoomMapGen.h"
#include "Components/PointLightComponent.h"
#include "Perception/AISense_Hearing.h"
#include "DoomDehacked.h"
#include "Misc/FileHelper.h"
#include "Components/AudioComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/DirectionalLight.h"
#include "Components/SkyLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "DoomSpriteComponent.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Tasks/Task.h"

// ------------------------------------------------------------------------------------------------
// Line activation
// ------------------------------------------------------------------------------------------------

bool UDoomWorld::CheckLock(AActor* Activator, int32 Lock)
{
	if (Lock == 0)
	{
		return true;
	}
	ADoomPlayer* P = Cast<ADoomPlayer>(Activator);
	if (!P)
	{
		return false;
	}
	if (P->HasKey(Lock - 1))
	{
		return true;
	}
	static const TCHAR* Names[] = { TEXT("blue"), TEXT("yellow"), TEXT("red") };
	P->Message(FString::Printf(TEXT("You need a %s key to open this door."), Names[Lock - 1]));
	P->PlaySelfSound(TEXT("OOF"));
	return false;
}

void UDoomWorld::FlipSwitch(int32 LineIndex, int32 SideIdx, bool bRepeat)
{
	FDoomLine& L = Map->Lines[LineIndex];
	if (L.Side[SideIdx] == INDEX_NONE)
	{
		return;
	}
	FDoomSide& S = Map->Sides[L.Side[SideIdx]];
	FName* Parts[3] = { &S.Upper, &S.Middle, &S.Lower };
	for (int32 p = 0; p < 3; ++p)
	{
		const FName Partner = Gfx->SwitchPartner(*Parts[p]);
		if (Partner.IsNone())
		{
			continue;
		}
		const FVector2f C = (Map->V(L.V1) + Map->V(L.V2)) * 0.5f;
		const FVector Pos = Doom::ToUE(C, Map->Sectors[S.Sector].Floor + 32);
		PlaySound(L.Special == 11 ? TEXT("SWTCHX") : TEXT("SWTCHN"), &Pos);
		if (bRepeat)
		{
			// a switch already pushed in keeps its one timer (vanilla P_StartButton)
			if (Buttons.ContainsByPredicate([LineIndex](const FSwitchButton& B) { return B.Line == LineIndex; }))
			{
				return;
			}
			Buttons.Add({ LineIndex, SideIdx, p, *Parts[p], Doom::TicRate });
		}
		*Parts[p] = Partner;
		Level->LineChanged(LineIndex);
		return;
	}
}

bool UDoomWorld::ActivateSpecial(int32 LineIndex, int32 SideIdx, AActor* Activator, EActivation How, bool bForce)
{
	FDoomLine& L = Map->Lines[LineIndex];
	const DoomWorldDetail::FSpec* Spec = DoomWorldDetail::FindSpec(L.Special);
	if (!Spec)
	{
		return false;
	}
	const bool bMonster = Activator && Activator->IsA<ADoomMonster>();
	if (bMonster && !Spec->bMonster && !(How == EActivation::Shoot && L.Special == 46))
	{
		return false; // (a monster's bullet may open the GR door 46, as in P_ShootSpecialLine)
	}
	if (!bMonster && Spec->Act == DoomWorldDetail::EAct::TeleportMonsterOnly)
	{
		return false;
	}
	if (!bForce)
	{
		switch (How)
		{
		case EActivation::Use: if (Spec->Trig != 'S' && Spec->Trig != 'D') return false; break;
		case EActivation::Cross: if (Spec->Trig != 'W') return false; break;
		case EActivation::Shoot: if (Spec->Trig != 'G') return false; break;
		}
		if (bMonster && (L.Flags & DoomLineFlags::Secret))
		{
			return false;
		}
		if (!CheckLock(Activator, Spec->Lock))
		{
			return false;
		}
	}

	TArray<int32> Targets;
	if (Spec->Trig == 'D')
	{
		if (L.BackSector == INDEX_NONE)
		{
			return false;
		}
		Targets.Add(L.BackSector);
	}
	else if (Spec->Act != DoomWorldDetail::EAct::Exit && Spec->Act != DoomWorldDetail::EAct::SecretExit && Spec->Act != DoomWorldDetail::EAct::Teleport && Spec->Act != DoomWorldDetail::EAct::TeleportMonsterOnly)
	{
		Targets = Map->SectorsWithTag(L.Tag);
	}

	bool bDid = false;
	auto Door = [&](int32 Kind, float Speed)
	{
		for (int32 S : Targets)
		{
			if (FSectorMover* Existing = FindMover(S))
			{
				// manual doors: re-using an open door closes it, a closing one reopens
				if (Spec->Trig == 'D' && Existing->Type == FSectorMover::EType::Door)
				{
					if (Existing->Dir < 0) { Existing->Dir = 1; Existing->Dest = Existing->High; }
					else if (!bMonster && Existing->bReturn) { Existing->Dir = -1; Existing->Dest = Map->Sectors[S].Floor; }
					bDid = true;
				}
				continue;
			}
			AddDoor(S, Kind, Speed, Activator);
			bDid = true;
		}
	};
	auto ForFree = [&](TFunctionRef<void(int32)> Fn)
	{
		for (int32 S : Targets)
		{
			if (!SectorHasMover(S)) { Fn(S); bDid = true; }
		}
	};

	switch (Spec->Act)
	{
	case DoomWorldDetail::EAct::DoorRaise: Door(DoomWorldDetail::DK_Raise, DoomWorldDetail::DoorSpeed); break;
	case DoomWorldDetail::EAct::DoorOpen: Door(DoomWorldDetail::DK_Open, DoomWorldDetail::DoorSpeed); break;
	case DoomWorldDetail::EAct::DoorClose: Door(DoomWorldDetail::DK_Close, DoomWorldDetail::DoorSpeed); break;
	case DoomWorldDetail::EAct::DoorClose30: Door(DoomWorldDetail::DK_Close30, DoomWorldDetail::DoorSpeed); break;
	case DoomWorldDetail::EAct::DoorBlazeRaise: Door(DoomWorldDetail::DK_Raise, DoomWorldDetail::DoorSpeed * 4); break;
	case DoomWorldDetail::EAct::DoorBlazeOpen: Door(DoomWorldDetail::DK_Open, DoomWorldDetail::DoorSpeed * 4); break;
	case DoomWorldDetail::EAct::DoorBlazeClose: Door(DoomWorldDetail::DK_Close, DoomWorldDetail::DoorSpeed * 4); break;
	case DoomWorldDetail::EAct::Lift: ForFree([&](int32 S) { AddPlat(S, DoomWorldDetail::PK_Lift, 4, 0); }); break;
	case DoomWorldDetail::EAct::LiftBlaze: ForFree([&](int32 S) { AddPlat(S, DoomWorldDetail::PK_Lift, 8, 0); }); break;
	case DoomWorldDetail::EAct::PlatPerpetual:
		for (FSectorMover& M : Movers)
		{
			if (M.bPerpetual && M.bStasis && Targets.Contains(M.Sector)) { M.bStasis = false; bDid = true; }
		}
		ForFree([&](int32 S) { AddPlat(S, DoomWorldDetail::PK_Perpetual, 1, 0); }); // PLATSPEED
		break;
	case DoomWorldDetail::EAct::PlatStop:
		for (FSectorMover& M : Movers)
		{
			if (M.bPerpetual && Targets.Contains(M.Sector)) { M.bStasis = true; bDid = true; }
		}
		break;
	case DoomWorldDetail::EAct::PlatRaise24Change:
	case DoomWorldDetail::EAct::PlatRaise32Change:
	case DoomWorldDetail::EAct::PlatRaiseNextChange:
		ForFree([&](int32 S)
		{
			if (Spec->Act == DoomWorldDetail::EAct::PlatRaiseNextChange) AddPlat(S, DoomWorldDetail::PK_RaiseNextChange, 0.5f, 0);
			else AddPlat(S, DoomWorldDetail::PK_RaiseChange, 0.5f, Spec->Act == DoomWorldDetail::EAct::PlatRaise24Change ? 24 : 32);
			// EV_DoPlat raiseAndChange / raiseToNearestAndChange: the floor takes the texture (and loses
			// its special, for the "next" kinds) as it starts, so a nukage bridge stops hurting at once
			FSectorMover& NewPlat = Movers.Last();
			if (L.FrontSector != INDEX_NONE)
			{
				Map->Sectors[S].FloorPic = Map->Sectors[L.FrontSector].FloorPic;
			}
			if (NewPlat.ChangeSpecial >= 0)
			{
				Map->Sectors[S].Special = NewPlat.ChangeSpecial;
			}
			NewPlat.ChangeFloorPic = NAME_None;
			NewPlat.ChangeSpecial = -1;
			Level->SectorChanged(S);
		});
		break;
	case DoomWorldDetail::EAct::FloorRaiseLowestCeil: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_RaiseLowestCeil, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorRaiseCrush: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_RaiseCrush, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorRaiseNext: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_RaiseNext, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorRaiseNextTurbo: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_RaiseNext, 4, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorLowerHighest: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_LowerHighest, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorLowerHighestTurbo: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_LowerHighestPlus8, 4, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorLowerLowest: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_LowerLowest, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorLowerLowestChange: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_LowerLowestChange, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorRaise24: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_Raise24, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorRaise24Change: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_Raise24Change, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorRaise512: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_Raise512, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::FloorRaiseShortest: ForFree([&](int32 S) { AddFloor(S, DoomWorldDetail::FK_RaiseShortest, 1, LineIndex); }); break;
	case DoomWorldDetail::EAct::Donut:
		for (int32 S1 : Targets)
		{
			if (SectorHasMover(S1) || Map->Sectors[S1].Lines.Num() == 0) continue;
			const int32 Ring = Map->OtherSector(Map->Sectors[S1].Lines[0], S1);
			if (Ring == INDEX_NONE) continue;
			for (int32 Li : Map->Sectors[Ring].Lines)
			{
				const int32 Outer = Map->OtherSector(Li, Ring);
				if (Outer == INDEX_NONE || Outer == S1) continue;
				const float H = Map->Sectors[Outer].Floor;
				FSectorMover R; R.Type = FSectorMover::EType::Floor; R.Sector = Ring; R.Speed = 0.5f; R.Dir = 1; R.Dest = H;
				R.ChangeFloorPic = Map->Sectors[Outer].FloorPic; R.ChangeSpecial = 0;
				Movers.Add(R);
				FSectorMover P; P.Type = FSectorMover::EType::Floor; P.Sector = S1; P.Speed = 0.5f; P.Dir = -1; P.Dest = H;
				Movers.Add(P);
				bDid = true;
				break;
			}
		}
		break;
	case DoomWorldDetail::EAct::CeilRaiseHighest: ForFree([&](int32 S) { AddCeiling(S, DoomWorldDetail::CK_RaiseHighest, 1); }); break;
	case DoomWorldDetail::EAct::CeilLowerFloor: ForFree([&](int32 S) { AddCeiling(S, DoomWorldDetail::CK_LowerFloor, 1); }); break;
	case DoomWorldDetail::EAct::CeilLower8: ForFree([&](int32 S) { AddCeiling(S, DoomWorldDetail::CK_Lower8, 1); }); break;
	case DoomWorldDetail::EAct::Crusher:
	case DoomWorldDetail::EAct::CrusherFast:
	case DoomWorldDetail::EAct::CrusherSilent:
		for (FSectorMover& M : Movers)
		{
			if (M.Type == FSectorMover::EType::Crusher && M.bStasis && Targets.Contains(M.Sector)) { M.bStasis = false; bDid = true; }
		}
		ForFree([&](int32 S)
		{
			AddCeiling(S, Spec->Act == DoomWorldDetail::EAct::CrusherSilent ? DoomWorldDetail::CK_CrusherSilent : DoomWorldDetail::CK_Crusher, Spec->Act == DoomWorldDetail::EAct::CrusherFast ? 2.f : 1.f);
		});
		break;
	case DoomWorldDetail::EAct::CrusherStop:
		for (FSectorMover& M : Movers)
		{
			if (M.Type == FSectorMover::EType::Crusher && Targets.Contains(M.Sector)) { M.bStasis = true; bDid = true; }
		}
		break;
	case DoomWorldDetail::EAct::Stairs8: for (int32 S : Targets) if (!SectorHasMover(S)) { BuildStairs(S, 8, 0.25f); bDid = true; } break;
	case DoomWorldDetail::EAct::Stairs16: for (int32 S : Targets) if (!SectorHasMover(S)) { BuildStairs(S, 16, 4); bDid = true; } break;
	case DoomWorldDetail::EAct::LightMaxNeighbour:
		for (int32 S : Targets) { Map->Sectors[S].Light = Map->MaxSurroundingLight(S, 0); }
		bDid = bLightsDirty = true;
		break;
	case DoomWorldDetail::EAct::LightMinNeighbour:
		for (int32 S : Targets) { Map->Sectors[S].Light = Map->MinSurroundingLight(S, Map->Sectors[S].Light); }
		bDid = bLightsDirty = true;
		break;
	case DoomWorldDetail::EAct::Light255: for (int32 S : Targets) Map->Sectors[S].Light = 255; bDid = bLightsDirty = true; break;
	case DoomWorldDetail::EAct::Light35: for (int32 S : Targets) Map->Sectors[S].Light = 35; bDid = bLightsDirty = true; break;
	case DoomWorldDetail::EAct::LightStrobe:
		for (int32 S : Targets)
		{
			FLightEffect E;
			E.Type = FLightEffect::EType::StrobeSlow; E.Sector = S; E.Max = Map->Sectors[S].Light;
			E.Min = Map->MinSurroundingLight(S, E.Max); if (E.Min == E.Max) E.Min = 0;
			E.DarkTime = 35; E.BrightTime = 5; E.Count = 1;
			Lights.Add(E);
		}
		bDid = true;
		break;
	case DoomWorldDetail::EAct::Teleport:
	case DoomWorldDetail::EAct::TeleportMonsterOnly:
		if (SideIdx == 0)
		{
			Teleport(Activator, LineIndex, SideIdx);
			bDid = true;
		}
		break;
	case DoomWorldDetail::EAct::Exit:
		if (Activator && Activator->IsA<ADoomPlayer>()) { if (How == EActivation::Use) FlipSwitch(LineIndex, SideIdx, false); ExitLevel(false); return true; }
		break;
	case DoomWorldDetail::EAct::SecretExit:
		if (Activator && Activator->IsA<ADoomPlayer>()) { if (How == EActivation::Use) FlipSwitch(LineIndex, SideIdx, false); ExitLevel(true); return true; }
		break;
	}

	if (!bDid && Spec->Trig != 'D')
	{
		// vanilla P_CrossSpecialLine uses up a W1 line even when its sectors were busy
		if (Spec->Trig == 'W' && !Spec->bRepeat)
		{
			L.Special = 0;
		}
		return false;
	}
	if (Spec->Trig == 'S' || Spec->Trig == 'G')
	{
		FlipSwitch(LineIndex, SideIdx, Spec->bRepeat);
	}
	if (!Spec->bRepeat)
	{
		L.Special = 0;
	}
	return true;
}

void UDoomWorld::UseLines(AActor* User, const FVector& From, float DoomAngle)
{
	if (!Map.IsValid())
	{
		return;
	}
	const FVector2f A = Doom::ToDoom2D(From);
	const float Rad = FMath::DegreesToRadians(DoomAngle);
	const FVector2f B = A + FVector2f(FMath::Cos(Rad), FMath::Sin(Rad)) * 64.f;
	TArray<FLineHit> Hits;
	Map->TraceLines(A, B, Hits);
	for (const FLineHit& H : Hits)
	{
		FDoomLine& L = Map->Lines[H.Line];
		if (L.Special != 0)
		{
			// P_UseSpecialLine: nothing works from a line's back side, doors included (you can't
			// toggle a door from inside it)
			if (H.Side == 1)
			{
				return;
			}
			ActivateSpecial(H.Line, H.Side, User, EActivation::Use);
			return;
		}
		float Top, Bottom;
		if (!Map->LineOpening(H.Line, Top, Bottom))
		{
			PlaySound(TEXT("NOWAY"), nullptr);
			return;
		}
	}
}

bool UDoomWorld::TryMonsterUse(ADoomMonster* Monster, const FVector2f& From, const FVector2f& To)
{
	TArray<FLineHit> Hits;
	Map->TraceLines(From, To, Hits);
	for (const FLineHit& H : Hits)
	{
		const FDoomLine& L = Map->Lines[H.Line];
		if (L.Special != 0)
		{
			const DoomWorldDetail::FSpec* S = DoomWorldDetail::FindSpec(L.Special);
			if (S && S->bMonster && S->Trig == 'D' && H.Side == 0)
			{
				return ActivateSpecial(H.Line, H.Side, Monster, EActivation::Use);
			}
		}
	}
	return false;
}

void UDoomWorld::CheckCrossings(AActor* Actor, const FVector2f& From, const FVector2f& To, bool bMonster)
{
	if (!Map.IsValid() || From.Equals(To, 0.001f))
	{
		return;
	}
	// copy: activation can clear specials
	const TArray<int32> Lines = Map->SpecialLines;
	for (int32 Li : Lines)
	{
		const FDoomLine& L = Map->Lines[Li];
		if (L.Special == 0)
		{
			continue;
		}
		int32 Side;
		if (!Map->SegmentCrossesLine(From, To, Li, Side))
		{
			continue;
		}
		const DoomWorldDetail::FSpec* S = DoomWorldDetail::FindSpec(L.Special);
		if (!S || S->Trig != 'W')
		{
			continue;
		}
		ActivateSpecial(Li, Side, Actor, EActivation::Cross);
		if (Flow().Phase != EDoomGameState::Playing)
		{
			return;
		}
		if (!IsValid(Actor) || FVector2f::Distance(Doom::ToDoom2D(Actor->GetActorLocation()), To) > 1.f)
		{
			return; // teleported: the rest of this move's lines no longer apply
		}
	}
}

void UDoomWorld::CheckShootLines(AActor* Shooter, const FVector2f& From, const FVector2f& To)
{
	TArray<FLineHit> Hits;
	Map->TraceLines(From, To, Hits);
	for (const FLineHit& H : Hits)
	{
		const FDoomLine& L = Map->Lines[H.Line];
		if (L.Special != 0)
		{
			const DoomWorldDetail::FSpec* S = DoomWorldDetail::FindSpec(L.Special);
			if (S && S->Trig == 'G')
			{
				ActivateSpecial(H.Line, H.Side, Shooter, EActivation::Shoot);
				return;
			}
		}
		float Top, Bottom;
		if (!Map->LineOpening(H.Line, Top, Bottom))
		{
			return;
		}
	}
}

void UDoomWorld::Teleport(AActor* Actor, int32 LineIndex, int32 Side)
{
	if (!Actor)
	{
		return;
	}
	const int32 Tag = Map->Lines[LineIndex].Tag;
	for (const FDoomMapThing& T : Map->Things)
	{
		if (T.Type != 14)
		{
			continue;
		}
		const int32 Sec = Map->SectorAt(T.P);
		if (Sec < 0 || Map->Sectors[Sec].Tag != Tag)
		{
			continue;
		}
		const FVector OldPos = Actor->GetActorLocation();
		const float Half = Doom::HalfHeight(Actor);
		const FVector NewPos = Doom::ToUE(T.P, Map->Sectors[Sec].Floor) + FVector(0, 0, Half + 1);
		// P_TeleportMove: a player telefrags whatever stands on the spot; a monster can't teleport onto
		// someone (except on MAP30, where the boss shooter's cubes telefrag)
		ADoomMonster* Self = Cast<ADoomMonster>(Actor);
		const float MyRadius = Self ? Self->Def->Radius : 16.f;
		const bool bCanFrag = !Self || GetMapName() == TEXT("MAP30");
		TArray<AActor*> Victims;
		auto Overlaps = [&](const AActor* Other, float Radius)
		{
			const FVector2f D = Doom::ToDoom2D(Other->GetActorLocation()) - T.P;
			return FMath::Abs(D.X) < Radius + MyRadius && FMath::Abs(D.Y) < Radius + MyRadius;
		};
		for (ADoomPlayer* P : GetPlayers())
		{
			if (P != Actor && !P->IsDead() && Overlaps(P, 16.f)) Victims.Add(P);
		}
		for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
		{
			if (M.IsValid() && M.Get() != Actor && !M->IsDead() && Overlaps(M.Get(), M->Def->Radius)) Victims.Add(M.Get());
		}
		if (Victims.Num() && !bCanFrag)
		{
			return;
		}
		for (AActor* V : Victims)
		{
			UGameplayStatics::ApplyDamage(V, 10000.f, nullptr, Actor, nullptr);
		}
		Actor->SetActorLocation(NewPos, false, nullptr, ETeleportType::TeleportPhysics);
		if (ADoomPlayer* TP = Cast<ADoomPlayer>(Actor))
		{
			TP->ReactionTime = 18; // a short freeze after teleporting
		}
		if (ACharacter* C = Cast<ACharacter>(Actor))
		{
			C->GetCharacterMovement()->StopMovementImmediately();
			if (AController* Ctrl = C->GetController())
			{
				Ctrl->SetControlRotation(FRotator(0, Doom::AngleToYaw(T.Angle), 0));
			}
		}
		if (ADoomMonster* M = Cast<ADoomMonster>(Actor))
		{
			M->DoomAngle = T.Angle;
		}
		SpawnEffect(TEXT("TFOG"), TEXT("ABABCDEFGHIJ"), 6, OldPos);
		PlaySound(TEXT("TELEPT"), &OldPos);
		const float Rad = FMath::DegreesToRadians(T.Angle);
		const FVector Fwd = Doom::ToUE(FMath::Cos(Rad), FMath::Sin(Rad), 0).GetSafeNormal() * 20.f * Doom::Scale;
		SpawnEffect(TEXT("TFOG"), TEXT("ABABCDEFGHIJ"), 6, NewPos + Fwd);
		PlaySound(TEXT("TELEPT"), &NewPos);
		return;
	}
}

void UDoomWorld::NoiseAlert(AActor* Target, const FVector& Where)
{
	// SMART monsters also hear it through Unreal's AI perception (awake ones follow the sound)
	if (bSmartAI)
	{
		UAISense_Hearing::ReportNoiseEvent(this, Where, 1.f, Target);
	}
	// Flood the sound through open two-sided lines; a sound-blocking line may be crossed once.
	const int32 Start = SectorAt(Where);
	if (Start < 0)
	{
		return;
	}
	++ValidCount;
	struct FItem { int32 Sector; int32 Blocks; };
	TArray<FItem> Stack = { { Start, 0 } };
	TSet<int32> Heard;
	while (Stack.Num())
	{
		const FItem It = Stack.Pop();
		FDoomSector& S = Map->Sectors[It.Sector];
		if (S.SoundValid == ValidCount && S.SoundTraversed <= It.Blocks + 1)
		{
			continue;
		}
		S.SoundValid = ValidCount;
		S.SoundTraversed = It.Blocks + 1;
		S.SoundTarget = Target;
		Heard.Add(It.Sector);
		for (int32 Li : S.Lines)
		{
			const FDoomLine& L = Map->Lines[Li];
			float Top, Bottom;
			if (!Map->LineOpening(Li, Top, Bottom))
			{
				continue;
			}
			const int32 Other = Map->OtherSector(Li, It.Sector);
			if (Other == INDEX_NONE)
			{
				continue;
			}
			if (L.Flags & DoomLineFlags::SoundBlock)
			{
				if (It.Blocks == 0) Stack.Add({ Other, 1 });
			}
			else
			{
				Stack.Add({ Other, It.Blocks });
			}
		}
	}
	for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
	{
		if (M.IsValid() && M->State == EMonsterState::Idle && Heard.Contains(SectorAt(M->GetActorLocation())))
		{
			M->WakeUp(Target, true);
		}
	}
}

bool UDoomWorld::ExecuteSpecial(int32 Special, int32 Tag)
{
	if (!Map.IsValid())
	{
		return false;
	}
	// a line that exists only for this activation
	FDoomLine Fake;
	Fake.Special = (int16)Special;
	Fake.Tag = (int16)Tag;
	const int32 Index = Map->Lines.Add(Fake);
	const bool bDone = ActivateSpecial(Index, 0, GetPlayer(), EActivation::Use, true);
	if (Map.IsValid() && Map->Lines.IsValidIndex(Index))
	{
		Map->Lines.RemoveAt(Index);
	}
	return bDone;
}

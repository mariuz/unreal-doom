// UDoomWorld: the sector movers (doors, lifts, floors, ceilings, crushers, stairs), lights and
// damaging floors. The rest of the world is in DoomWorld.cpp and its DoomWorld*.cpp siblings.
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
// Sector movers
// ------------------------------------------------------------------------------------------------

bool UDoomWorld::SectorHasMover(int32 Sector) const
{
	return Movers.ContainsByPredicate([Sector](const FSectorMover& M) { return M.Sector == Sector && !M.bDone; });
}

FSectorMover* UDoomWorld::FindMover(int32 Sector)
{
	return Movers.FindByPredicate([Sector](const FSectorMover& M) { return M.Sector == Sector && !M.bDone; });
}

bool UDoomWorld::ThingsBlockPlane(int32 Sector, float Floor, float Ceiling, bool bCrush)
{
	bool bBlocked = false;
	const FDoomSector& Sec = Map->Sectors[Sector];
	auto Touches = [&](const FVector2f& P, float Radius)
	{
		if (Map->SectorAt(P) == Sector) return true;
		for (int32 L : Sec.Lines)
		{
			if (Map->DistanceToLine(P, L) < Radius) return true;
		}
		return false;
	};
	auto Check = [&](AActor* A, float Height, float Radius, bool bSolid)
	{
		if (!A || !bSolid)
		{
			return;
		}
		const FVector2f P = Doom::ToDoom2D(A->GetActorLocation());
		if (!Touches(P, Radius))
		{
			return;
		}
		if (Ceiling - Floor < Height)
		{
			bBlocked = true;
			if (bCrush && (Flow().LevelTic & 3) == 0)
			{
				UGameplayStatics::ApplyDamage(A, 10.f, nullptr, nullptr, nullptr);
			}
		}
	};
	for (ADoomPlayer* P : GetPlayers())
	{
		Check(P, P->IsDead() ? 0.f : 56.f, 16.f, true);
	}
	for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
	{
		if (M.IsValid() && !M->IsDead()) Check(M.Get(), M->Def->Height, M->Def->Radius, true);
	}
	for (const TWeakObjectPtr<ADoomThing>& T : Things)
	{
		if (T.IsValid() && T->Def && (T->Def->Flags & EThingFlag::Solid) && !T->bDead) Check(T.Get(), T->Def->Height, T->Def->Radius, true);
	}
	return bBlocked;
}

bool UDoomWorld::MovePlane(FSectorMover& M, bool& bBlocked)
{
	// Returns true when the destination was reached; bBlocked when a non-crushing plane could not move
	// because something is in the way (vanilla P_MovePlane's "crushed").
	bBlocked = false;
	FDoomSector& S = Map->Sectors[M.Sector];
	float& H = M.bCeiling ? S.Ceiling : S.Floor;
	const float Old = H;
	float New = H + M.Speed * M.Dir;
	bool bArrived = false;
	if ((M.Dir > 0 && New >= M.Dest) || (M.Dir < 0 && New <= M.Dest))
	{
		New = M.Dest;
		bArrived = true;
	}
	// crushing / blocking: ceiling down or floor up squeezes things
	const bool bSqueezing = (M.bCeiling && M.Dir < 0) || (!M.bCeiling && M.Dir > 0);
	if (bSqueezing)
	{
		const float NF = M.bCeiling ? S.Floor : New;
		const float NC = M.bCeiling ? New : S.Ceiling;
		if (ThingsBlockPlane(M.Sector, NF, NC, M.bCrush))
		{
			if (!M.bCrush)
			{
				bBlocked = true;
				return false; // caller decides (doors reverse)
			}
			if (M.Speed <= 1.f && M.Type == FSectorMover::EType::Crusher)
			{
				New = Old + 0.125f * M.Dir; // slow crushers grind
			}
		}
	}
	H = New;
	Level->SectorChanged(M.Sector);

	// lift things standing on a rising floor; let them follow a falling one
	if (!M.bCeiling)
	{
		auto Lift = [&](ACharacter* C)
		{
			if (!C) return;
			if (Map->SectorAt(Doom::ToDoom2D(C->GetActorLocation())) != M.Sector) return;
			const float Half = Doom::HalfHeight(C);
			const float FeetZ = Doom::ToDoomZ(C->GetActorLocation().Z - Half);
			if (M.Dir > 0 && FeetZ < New + 0.5f)
			{
				FVector L = C->GetActorLocation();
				L.Z = New * Doom::Scale + Half + 1.f;
				C->SetActorLocation(L, false, nullptr, ETeleportType::None);
				if (ADoomPlayer* P = Cast<ADoomPlayer>(C))
				{
					P->LastFeetZ = P->DoomZ(); // carried up, not a step: the view doesn't dip
				}
			}
		};
		for (ADoomPlayer* P : GetPlayers()) Lift(P);
		if (M.Dir < 0)
		{
			// a lowering floor: resting corpses on it fall with it (their movement ticks again)
			for (const TWeakObjectPtr<ADoomMonster>& Mon : Monsters)
			{
				if (!Mon.IsValid() || Map->SectorAt(Mon->DoomPos()) != M.Sector) continue;
				if (Mon->IsCorpseAsleep()) Mon->WakeCorpse();
				if (Mon->IsIdleAsleep()) Mon->WakeIdle();
			}
		}
		for (const TWeakObjectPtr<ADoomMonster>& Mon : Monsters)
		{
			if (Mon.IsValid()) Lift(Mon.Get());
		}
		for (const TWeakObjectPtr<ADoomThing>& T : Things)
		{
			if (T.IsValid() && !T->bEffect && Map->SectorAt(Doom::ToDoom2D(T->GetActorLocation())) == M.Sector) T->SnapToPlanes();
		}
	}
	if (!M.bSilent && M.Type != FSectorMover::EType::Door && M.Type != FSectorMover::EType::Plat && (Flow().LevelTic & 7) == 0)
	{
		const FVector Pos = Doom::ToUE(S.Center, S.Floor + 32);
		PlaySound(TEXT("STNMOV"), &Pos, 0.6f);
	}
	return bArrived;
}

void UDoomWorld::TicMovers()
{
	for (int32 i = 0; i < Movers.Num(); ++i)
	{
		// a copy, written back at the end: a crusher can kill the last boss, whose special starts new
		// movers, and the array may grow (reallocate) under a reference
		FSectorMover M = Movers[i];
		ON_SCOPE_EXIT { Movers[i] = M; };
		if (M.bDone || M.bStasis)
		{
			continue;
		}
		FDoomSector& S = Map->Sectors[M.Sector];
		const FVector SoundPos = Doom::ToUE(S.Center, (S.Floor + S.Ceiling) * 0.5f);
		if (M.Dir == 0)
		{
			// waiting
			if (--M.Counter > 0)
			{
				continue;
			}
			switch (M.Type)
			{
			case FSectorMover::EType::Door:
				if (M.CloseThenOpenWait)
				{
					M.Dir = 1; M.Dest = M.High; M.CloseThenOpenWait = 0;
					PlaySound(M.bBlaze ? TEXT("BDOPN") : TEXT("DOROPN"), &SoundPos);
				}
				else
				{
					M.Dir = -1; M.Dest = S.Floor;
					PlaySound(M.bBlaze ? TEXT("BDCLS") : TEXT("DORCLS"), &SoundPos);
				}
				break;
			case FSectorMover::EType::Plat:
				M.Dir = (S.Floor <= M.Low + 0.01f) ? 1 : -1;
				M.Dest = M.Dir > 0 ? M.High : M.Low;
				PlaySound(TEXT("PSTART"), &SoundPos);
				break;
			default:
				M.bDone = true;
				break;
			}
			continue;
		}

		bool bBlocked = false;
		const bool bArrived = MovePlane(M, bBlocked);

		switch (M.Type)
		{
		case FSectorMover::EType::Door:
			if (bArrived)
			{
				if (M.Dir > 0)
				{
					if (M.bReturn) { M.Dir = 0; M.Counter = M.Wait; }
					else M.bDone = true;
				}
				else
				{
					if (M.bBlaze)
					{
						PlaySound(TEXT("BDCLS"), &SoundPos); // vanilla: only blazing doors sound again when shut
					}
					if (M.CloseThenOpenWait) { M.Dir = 0; M.Counter = M.CloseThenOpenWait; }
					else M.bDone = true;
				}
			}
			else if (M.Dir < 0 && bBlocked && (M.bReturn || M.CloseThenOpenWait))
			{
				// something is in the way: go back up (plain close doors wait instead, as in vanilla)
				M.Dir = 1; M.Dest = M.High;
				PlaySound(M.bBlaze ? TEXT("BDOPN") : TEXT("DOROPN"), &SoundPos);
			}
			break;
		case FSectorMover::EType::Plat:
			if (bBlocked && M.Dir > 0)
			{
				M.Dir = -1; M.Dest = M.Low;
				PlaySound(TEXT("PSTART"), &SoundPos);
			}
			else if (bArrived)
			{
				PlaySound(TEXT("PSTOP"), &SoundPos);
				if (!M.ChangeFloorPic.IsNone())
				{
					S.FloorPic = M.ChangeFloorPic;
					if (M.ChangeSpecial >= 0) S.Special = M.ChangeSpecial;
					Level->SectorChanged(M.Sector);
				}
				if (M.bPerpetual || (M.bReturn && M.Dir < 0))
				{
					M.Dir = 0; M.Counter = M.Wait;
				}
				else
				{
					M.bDone = true;
				}
			}
			break;
		case FSectorMover::EType::Crusher:
			if (bArrived)
			{
				if (M.Dir < 0) { M.Dir = 1; M.Dest = M.High; }
				else { M.Dir = -1; M.Dest = M.Low; }
				if (M.bSilent) PlaySound(TEXT("PSTOP"), &SoundPos); // vanilla: only the silent crusher (141) clanks at its ends
			}
			break;
		default: // floors, ceilings
			if (bArrived || bBlocked)
			{
				if (bArrived && !M.ChangeFloorPic.IsNone())
				{
					S.FloorPic = M.ChangeFloorPic;
					if (M.ChangeSpecial >= 0) S.Special = M.ChangeSpecial;
					Level->SectorChanged(M.Sector);
				}
				if (bArrived && !M.bCeiling)
				{
					PlaySound(TEXT("PSTOP"), &SoundPos); // floors stop with a clank; ceilings stop silently
				}
				M.bDone = bArrived;
			}
			break;
		}
	}
	Movers.RemoveAll([](const FSectorMover& M) { return M.bDone; });
}

void UDoomWorld::AddDoor(int32 Sector, int32 Kind, float Speed, AActor* Activator)
{
	FDoomSector& S = Map->Sectors[Sector];
	FSectorMover M;
	M.Type = FSectorMover::EType::Door;
	M.Sector = Sector;
	M.bCeiling = true;
	M.Speed = Speed;
	M.bBlaze = Speed > DoomWorldDetail::DoorSpeed;
	M.Wait = DoomWorldDetail::DoorWait;
	M.High = Map->LowestCeilingSurrounding(Sector) - 4.f;
	const FVector Pos = Doom::ToUE(S.Center, (S.Floor + S.Ceiling) * 0.5f);
	switch (Kind)
	{
	case DoomWorldDetail::DK_Raise:
		M.bReturn = true; M.Dir = 1; M.Dest = M.High;
		if (S.Ceiling >= M.High) { M.Dir = 0; M.Counter = M.Wait; }
		PlaySound(M.bBlaze ? TEXT("BDOPN") : TEXT("DOROPN"), &Pos);
		break;
	case DoomWorldDetail::DK_Open:
		M.Dir = 1; M.Dest = M.High;
		if (S.Ceiling >= M.High) return;
		PlaySound(M.bBlaze ? TEXT("BDOPN") : TEXT("DOROPN"), &Pos);
		break;
	case DoomWorldDetail::DK_Close:
		M.Dir = -1; M.Dest = S.Floor;
		PlaySound(M.bBlaze ? TEXT("BDCLS") : TEXT("DORCLS"), &Pos);
		break;
	case DoomWorldDetail::DK_Close30:
		M.Dir = -1; M.Dest = S.Floor; M.CloseThenOpenWait = 30 * Doom::TicRate; M.High = S.Ceiling;
		PlaySound(TEXT("DORCLS"), &Pos);
		break;
	case DoomWorldDetail::DK_CloseIn30:
		// sector 10 (P_SpawnDoorCloseIn30): a normal door, open now, that closes after 30 s; blocked, it
		// reopens, waits and closes again like any door. No sound at load.
		M.bReturn = true; M.Dir = 0; M.Counter = 30 * Doom::TicRate; M.High = S.Ceiling;
		break;
	case DoomWorldDetail::DK_RaiseIn5Min:
		// opens after 5 minutes, then behaves as a normal door (waits, closes)
		M.Dir = 0; M.Counter = 5 * 60 * Doom::TicRate; M.CloseThenOpenWait = 1; M.Dest = M.High; M.bReturn = true;
		break;
	}
	Movers.Add(M);
}

void UDoomWorld::AddPlat(int32 Sector, int32 Kind, float Speed, int32 Amount)
{
	FDoomSector& S = Map->Sectors[Sector];
	FSectorMover M;
	M.Type = FSectorMover::EType::Plat;
	M.Sector = Sector;
	M.Speed = Speed;
	M.Wait = DoomWorldDetail::PlatWait;
	const FVector Pos = Doom::ToUE(S.Center, S.Floor);
	switch (Kind)
	{
	case DoomWorldDetail::PK_Lift:
		M.Low = FMath::Min(Map->LowestFloorSurrounding(Sector), S.Floor);
		M.High = S.Floor;
		M.Dir = -1; M.Dest = M.Low; M.bReturn = true;
		PlaySound(TEXT("PSTART"), &Pos);
		break;
	case DoomWorldDetail::PK_Perpetual:
		M.Low = FMath::Min(Map->LowestFloorSurrounding(Sector), S.Floor);
		M.High = FMath::Max(Map->HighestFloorSurrounding(Sector), S.Floor);
		M.bPerpetual = true;
		M.Dir = (DoomInfo::Random() & 1) ? 1 : -1;
		M.Dest = M.Dir > 0 ? M.High : M.Low;
		PlaySound(TEXT("PSTART"), &Pos);
		break;
	case DoomWorldDetail::PK_RaiseChange:
		M.Low = S.Floor; M.High = S.Floor + Amount;
		M.Dir = 1; M.Dest = M.High; M.Speed = 0.5f; M.bSilent = false;
		break;
	case DoomWorldDetail::PK_RaiseNextChange:
		M.Low = S.Floor; M.High = Map->NextHighestFloor(Sector, S.Floor);
		M.Dir = 1; M.Dest = M.High; M.Speed = 0.5f; M.ChangeSpecial = 0;
		break;
	}
	Movers.Add(M);
}

void UDoomWorld::AddFloor(int32 Sector, int32 Kind, float Speed, int32 LineIndex)
{
	FDoomSector& S = Map->Sectors[Sector];
	FSectorMover M;
	M.Type = FSectorMover::EType::Floor;
	M.Sector = Sector;
	M.Speed = Speed;
	switch (Kind)
	{
	case DoomWorldDetail::FK_RaiseLowestCeil: M.Dir = 1; M.Dest = FMath::Min(Map->LowestCeilingSurrounding(Sector), S.Ceiling); break;
	case DoomWorldDetail::FK_RaiseCrush: M.Dir = 1; M.Dest = FMath::Min(Map->LowestCeilingSurrounding(Sector), S.Ceiling) - 8; M.bCrush = true; break;
	case DoomWorldDetail::FK_RaiseNext: M.Dir = 1; M.Dest = Map->NextHighestFloor(Sector, S.Floor); break;
	case DoomWorldDetail::FK_LowerHighest: M.Dir = -1; M.Dest = Map->HighestFloorSurrounding(Sector); break;
	case DoomWorldDetail::FK_LowerHighestPlus8:
		M.Dir = -1; M.Dest = Map->HighestFloorSurrounding(Sector);
		if (M.Dest != S.Floor) M.Dest += 8;
		break;
	case DoomWorldDetail::FK_LowerLowest: M.Dir = -1; M.Dest = Map->LowestFloorSurrounding(Sector); break;
	case DoomWorldDetail::FK_LowerLowestChange:
	{
		M.Dir = -1; M.Dest = Map->LowestFloorSurrounding(Sector);
		for (int32 L : S.Lines)
		{
			const int32 O = Map->OtherSector(L, Sector);
			if (O != INDEX_NONE && Map->Sectors[O].Floor == M.Dest)
			{
				M.ChangeFloorPic = Map->Sectors[O].FloorPic;
				M.ChangeSpecial = Map->Sectors[O].Special;
				break;
			}
		}
		break;
	}
	case DoomWorldDetail::FK_Raise24: M.Dir = 1; M.Dest = S.Floor + 24; break;
	case DoomWorldDetail::FK_Raise24Change:
		M.Dir = 1; M.Dest = S.Floor + 24;
		// raiseFloor24AndChange changes the texture and special as it starts
		if (LineIndex >= 0 && Map->Lines[LineIndex].FrontSector != INDEX_NONE)
		{
			S.FloorPic = Map->Sectors[Map->Lines[LineIndex].FrontSector].FloorPic;
			S.Special = Map->Sectors[Map->Lines[LineIndex].FrontSector].Special;
			Level->SectorChanged(Sector);
		}
		break;
	case DoomWorldDetail::FK_Raise512: M.Dir = 1; M.Dest = S.Floor + 512; break;
	case DoomWorldDetail::FK_RaiseShortest:
	{
		int32 Shortest = 32000;
		for (int32 L : S.Lines)
		{
			const FDoomLine& Li = Map->Lines[L];
			if (Li.Side[0] == INDEX_NONE || Li.Side[1] == INDEX_NONE) continue;
			for (int32 s = 0; s < 2; ++s)
			{
				const int32 H = Gfx->TextureHeight(Map->Sides[Li.Side[s]].Lower);
				if (H > 0) Shortest = FMath::Min(Shortest, H);
			}
		}
		M.Dir = 1; M.Dest = S.Floor + (Shortest == 32000 ? 0 : Shortest);
		break;
	}
	}
	if (FMath::IsNearlyEqual(M.Dest, S.Floor))
	{
		return;
	}
	Movers.Add(M);
}

void UDoomWorld::AddCeiling(int32 Sector, int32 Kind, float Speed)
{
	FDoomSector& S = Map->Sectors[Sector];
	FSectorMover M;
	M.Sector = Sector;
	M.bCeiling = true;
	M.Speed = Speed;
	M.Type = FSectorMover::EType::Ceiling;
	switch (Kind)
	{
	case DoomWorldDetail::CK_RaiseHighest: M.Dir = 1; M.Dest = Map->HighestCeilingSurrounding(Sector); break;
	case DoomWorldDetail::CK_LowerFloor: M.Dir = -1; M.Dest = S.Floor; break;
	case DoomWorldDetail::CK_Lower8: M.Dir = -1; M.Dest = S.Floor + 8; break;
	case DoomWorldDetail::CK_Crusher:
	case DoomWorldDetail::CK_CrusherSilent:
		M.Type = FSectorMover::EType::Crusher;
		M.High = S.Ceiling; M.Low = S.Floor + 8; M.Dir = -1; M.Dest = M.Low; M.bCrush = true;
		M.bSilent = Kind == DoomWorldDetail::CK_CrusherSilent;
		break;
	}
	Movers.Add(M);
}

void UDoomWorld::BuildStairs(int32 Sector, float StepSize, float Speed)
{
	int32 Cur = Sector;
	float Height = Map->Sectors[Cur].Floor + StepSize;
	const FName Pic = Map->Sectors[Cur].FloorPic;
	TSet<int32> Used;
	for (int32 Guard = 0; Guard < 256; ++Guard)
	{
		Used.Add(Cur);
		FSectorMover M;
		M.Type = FSectorMover::EType::Floor;
		M.Sector = Cur; M.Speed = Speed; M.Dir = 1; M.Dest = Height;
		Movers.Add(M);
		int32 Next = INDEX_NONE;
		for (int32 L : Map->Sectors[Cur].Lines)
		{
			const FDoomLine& Li = Map->Lines[L];
			if (Li.FrontSector == Cur && Li.BackSector != INDEX_NONE && !Used.Contains(Li.BackSector)
				&& Map->Sectors[Li.BackSector].FloorPic == Pic && !SectorHasMover(Li.BackSector))
			{
				Next = Li.BackSector;
				break;
			}
		}
		if (Next == INDEX_NONE)
		{
			break;
		}
		Cur = Next;
		Height += StepSize;
	}
}

// ------------------------------------------------------------------------------------------------
// Lights and sector specials
// ------------------------------------------------------------------------------------------------

void UDoomWorld::StartSectorSpecials()
{
	for (int32 i = 0; i < Map->Sectors.Num(); ++i)
	{
		FDoomSector& S = Map->Sectors[i];
		auto AddLight = [&](FLightEffect::EType Type, int32 Dark, bool bSync)
		{
			FLightEffect E;
			E.Type = Type;
			E.Sector = i;
			E.Max = S.Light;
			E.Min = Map->MinSurroundingLight(i, S.Light);
			// strobes with no darker neighbour go to black; flicker and glow stay steady (vanilla)
			if (E.Min == E.Max && Type != FLightEffect::EType::Flicker && Type != FLightEffect::EType::Glow) E.Min = 0;
			E.DarkTime = Dark;
			E.BrightTime = 5;
			E.Count = bSync ? 1 : (DoomInfo::Random() & 7) + 1;
			if (Type == FLightEffect::EType::FireFlicker) { E.Min = Map->MinSurroundingLight(i, S.Light) + 16; E.Count = 4; }
			if (Type == FLightEffect::EType::Glow) { E.Dir = -1; }
			Lights.Add(E);
		};
		switch (S.Special)
		{
		case 1: AddLight(FLightEffect::EType::Flicker, 0, false); break;
		case 2: AddLight(FLightEffect::EType::StrobeFast, 15, false); break;
		case 3: AddLight(FLightEffect::EType::StrobeSlow, 35, false); break;
		case 4: AddLight(FLightEffect::EType::StrobeFast, 15, false); break;
		case 8: AddLight(FLightEffect::EType::Glow, 0, false); break;
		case 9: ++Flow().TotalSecrets; break;
		case 10: AddDoor(i, DoomWorldDetail::DK_CloseIn30, DoomWorldDetail::DoorSpeed, nullptr); break;
		case 12: AddLight(FLightEffect::EType::StrobeSlow, 35, true); break;
		case 13: AddLight(FLightEffect::EType::StrobeFast, 15, true); break;
		case 14: AddDoor(i, DoomWorldDetail::DK_RaiseIn5Min, DoomWorldDetail::DoorSpeed, nullptr); break;
		case 17: AddLight(FLightEffect::EType::FireFlicker, 0, false); break;
		default: break;
		}
	}
}

void UDoomWorld::TicLights()
{
	for (FLightEffect& E : Lights)
	{
		FDoomSector& S = Map->Sectors[E.Sector];
		const int16 Before = S.Light;
		switch (E.Type)
		{
		case FLightEffect::EType::Flicker:
			if (--E.Count > 0) break;
			if (S.Light == E.Max) { S.Light = E.Min; E.Count = (DoomInfo::Random() & 7) + 1; }
			else { S.Light = E.Max; E.Count = (DoomInfo::Random() & 63) + 1; }
			break;
		case FLightEffect::EType::StrobeFast:
		case FLightEffect::EType::StrobeSlow:
			if (--E.Count > 0) break;
			if (S.Light == E.Min) { S.Light = E.Max; E.Count = E.BrightTime; }
			else { S.Light = E.Min; E.Count = E.DarkTime; }
			break;
		case FLightEffect::EType::Glow:
			if (E.Dir < 0)
			{
				S.Light -= 8;
				if (S.Light <= E.Min) { S.Light += 8; E.Dir = 1; }
			}
			else
			{
				S.Light += 8;
				if (S.Light >= E.Max) { S.Light -= 8; E.Dir = -1; }
			}
			break;
		case FLightEffect::EType::FireFlicker:
			if (--E.Count > 0) break;
			S.Light = FMath::Max<int32>(E.Max - (DoomInfo::Random() & 3) * 16, E.Min);
			E.Count = 4;
			break;
		}
		bLightsDirty |= S.Light != Before;
	}
}

void UDoomWorld::TicSectorDamage()
{
	for (ADoomPlayer* P : GetPlayers())
	{
		if (P->IsDead())
		{
			continue;
		}
		const int32 Sec = Map->SectorAt(P->DoomPos());
		if (Sec >= 0)
		{
			TicSectorDamage(P, Sec);
		}
	}
}

void UDoomWorld::TicSectorDamage(ADoomPlayer* P, int32 Sec)
{
	FDoomSector& S = Map->Sectors[Sec];
	if (P->DoomZ() > S.Floor + 1.f)
	{
		return;
	}
	const bool bTic = (Flow().LevelTic & 31) == 0;
	switch (S.Special)
	{
	case 5: if (bTic && !P->RadSuit) UGameplayStatics::ApplyDamage(P, 10, nullptr, nullptr, nullptr); break;
	case 7: if (bTic && !P->RadSuit) UGameplayStatics::ApplyDamage(P, 5, nullptr, nullptr, nullptr); break;
	case 4:
	case 16: if (bTic && (!P->RadSuit || DoomInfo::Random() < 5)) UGameplayStatics::ApplyDamage(P, 20, nullptr, nullptr, nullptr); break;
	case 9:
		CreditSecret(P);
		S.Special = 0;
		P->Message(TEXT("You found a secret area!"));
		P->PlaySelfSound(TEXT("GETPOW"));
		break;
	case 11:
		P->bGodMode = false;
		if (bTic) UGameplayStatics::ApplyDamage(P, 20, nullptr, nullptr, nullptr);
		if (P->Inv().Health <= 10) ExitLevel(false);
		break;
	default: break;
	}
}

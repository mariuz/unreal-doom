// UDoomWorld's save / load half: what Doom's p_saveg.c archives (sectors, lines,
// sides, thinkers, mobjs, the player), written through an FArchive.
#include "DoomWorld.h"
#include "DoomSaveGame.h"
#include "DoomLevel.h"
#include "DoomThing.h"
#include "DoomMonster.h"
#include "DoomProjectile.h"
#include "DoomPlayer.h"
#include "DoomUE.h"
#include "Kismet/GameplayStatics.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	template <typename E>
	void SerializeEnum(FArchive& Ar, E& Value)
	{
		uint8 V = (uint8)Value;
		Ar << V;
		Value = (E)V;
	}

	void SerializeMover(FArchive& Ar, FSectorMover& M)
	{
		SerializeEnum(Ar, M.Type);
		Ar << M.Sector << M.bCeiling << M.Speed << M.Dir << M.Dest << M.Low << M.High << M.Wait << M.Counter
			<< M.bReturn << M.bPerpetual << M.bStasis << M.bCrush << M.bSilent << M.bBlaze << M.Tag
			<< M.CloseThenOpenWait << M.ChangeFloorPic << M.ChangeSpecial << M.bDone;
	}

	void SerializeLight(FArchive& Ar, FLightEffect& L)
	{
		SerializeEnum(Ar, L.Type);
		Ar << L.Sector << L.Min << L.Max << L.Count << L.DarkTime << L.BrightTime << L.Dir;
	}

	void SerializeButton(FArchive& Ar, FSwitchButton& B)
	{
		Ar << B.Line << B.Side << B.Part << B.Original << B.Timer;
	}

	FVector FeetOf(const AActor* A)
	{
		return A->GetActorLocation() - FVector(0, 0, Doom::HalfHeight(A));
	}
}

FString UDoomWorld::SlotName(int32 Slot)
{
	return Slot <= 0 ? FString(TEXT("doom_quick")) : FString::Printf(TEXT("doom_slot%d"), Slot);
}

void UDoomWorld::SerializeState(FArchive& Ar)
{
	int32 Version = UDoomSaveGame::CurrentVersion;
	Ar << Version;
	Ar << Flow().LevelTic << Flow().Kills << Flow().Items << Flow().Secrets << Flow().TotalKills << Flow().TotalItems << Flow().TotalSecrets << bFullMap;
	// spawning saved monsters and things below counts them again; the saved totals win
	const int32 SavedTotalKills = Flow().TotalKills, SavedTotalItems = Flow().TotalItems;

	// sectors, lines, sides
	int32 NumSectors = Map->Sectors.Num(), NumLines = Map->Lines.Num(), NumSides = Map->Sides.Num();
	Ar << NumSectors << NumLines << NumSides;
	if (NumSectors != Map->Sectors.Num() || NumLines != Map->Lines.Num() || NumSides != Map->Sides.Num())
	{
		Ar.SetError();
		return;
	}
	for (FDoomSector& S : Map->Sectors)
	{
		Ar << S.Floor << S.Ceiling << S.Light << S.FloorPic << S.CeilingPic << S.Special << S.Tag << S.bSecretFound;
	}
	for (FDoomLine& L : Map->Lines)
	{
		Ar << L.Special << L.Flags << L.bSeen;
	}
	for (FDoomSide& S : Map->Sides)
	{
		Ar << S.Upper << S.Middle << S.Lower;
	}

	// thinkers
	int32 N = Movers.Num();
	Ar << N;
	if (Ar.IsLoading()) Movers.SetNum(N);
	for (FSectorMover& M : Movers) SerializeMover(Ar, M);
	N = Lights.Num();
	Ar << N;
	if (Ar.IsLoading()) Lights.SetNum(N);
	for (FLightEffect& L : Lights) SerializeLight(Ar, L);
	N = Buttons.Num();
	Ar << N;
	if (Ar.IsLoading()) Buttons.SetNum(N);
	for (FSwitchButton& B : Buttons) SerializeButton(Ar, B);

	// the player
	if (ADoomPlayer* P = GetPlayer())
	{
		P->DoomSerialize(Ar);
	}

	// monsters (targets as indices: 0 none, 1 the player, 2 + i monster i)
	TArray<ADoomMonster*> MonsterList;
	if (Ar.IsSaving())
	{
		for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
		{
			if (M.IsValid() && !M->IsActorBeingDestroyed()) MonsterList.Add(M.Get());
		}
	}
	N = MonsterList.Num();
	Ar << N;
	TArray<int32> TargetCodes;
	TargetCodes.SetNumZeroed(N);
	for (int32 i = 0; i < N; ++i)
	{
		int32 Type = 0;
		FVector Feet = FVector::ZeroVector;
		if (Ar.IsSaving())
		{
			ADoomMonster* M = MonsterList[i];
			Type = M->Def->DoomEdNum;
			Feet = FeetOf(M);
			AActor* T = M->Target.Get();
			const int32 TargetIndex = MonsterList.IndexOfByKey(Cast<ADoomMonster>(T));
			TargetCodes[i] = !T ? 0 : T == GetPlayer() ? 1 : TargetIndex != INDEX_NONE ? 2 + TargetIndex : 0;
		}
		Ar << Type << Feet << TargetCodes[i];
		if (Ar.IsLoading())
		{
			ADoomMonster* M = SpawnMonster(Type, Feet, 0);
			if (!M)
			{
				Ar.SetError();
				return;
			}
			MonsterList.Add(M);
		}
		MonsterList[i]->DoomSerialize(Ar);
		if (Ar.IsLoading())
		{
			// put the capsule exactly where it was (SpawnMonster adds a small lift)
			MonsterList[i]->SetActorLocation(Feet + FVector(0, 0, Doom::HalfHeight(MonsterList[i])), false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
	if (Ar.IsLoading())
	{
		for (int32 i = 0; i < N; ++i)
		{
			const int32 Code = TargetCodes[i];
			MonsterList[i]->Target = Code == 1 ? Cast<AActor>(GetPlayer()) : (Code >= 2 && MonsterList.IsValidIndex(Code - 2)) ? MonsterList[Code - 2] : nullptr;
		}
	}

	// things: decorations, pickups still lying around, intact barrels (effects are not saved)
	TArray<ADoomThing*> ThingList;
	if (Ar.IsSaving())
	{
		for (const TWeakObjectPtr<ADoomThing>& T : Things)
		{
			if (T.IsValid() && T->Def && !T->bEffect && !T->bDead && !T->IsActorBeingDestroyed()) ThingList.Add(T.Get());
		}
	}
	N = ThingList.Num();
	Ar << N;
	for (int32 i = 0; i < N; ++i)
	{
		int32 Type = 0;
		FVector Feet = FVector::ZeroVector;
		bool bWasDropped = false;
		if (Ar.IsSaving())
		{
			Type = ThingList[i]->Def->DoomEdNum;
			Feet = FeetOf(ThingList[i]);
			bWasDropped = ThingList[i]->bDropped;
		}
		Ar << Type << Feet << bWasDropped;
		if (Ar.IsLoading())
		{
			ADoomThing* T = SpawnThing(Type, Feet, 0, bWasDropped);
			if (!T)
			{
				Ar.SetError();
				return;
			}
			ThingList.Add(T);
		}
		ThingList[i]->DoomSerialize(Ar);
	}
	// missiles in flight (exploding ones finish in a few tics and are dropped); shooter and
	// homing target use the monster codes above
	auto ActorCode = [&](AActor* A)
	{
		const int32 Index = MonsterList.IndexOfByKey(Cast<ADoomMonster>(A));
		return !A ? 0 : A == GetPlayer() ? 1 : Index != INDEX_NONE ? 2 + Index : 0;
	};
	auto CodeActor = [&](int32 Code) -> AActor*
	{
		return Code == 1 ? Cast<AActor>(GetPlayer()) : (Code >= 2 && MonsterList.IsValidIndex(Code - 2)) ? MonsterList[Code - 2] : nullptr;
	};
	TArray<ADoomProjectile*> Missiles;
	if (Ar.IsSaving())
	{
		for (const TWeakObjectPtr<ADoomProjectile>& P : Projectiles)
		{
			if (P.IsValid() && P->Def && !P->bExploded && !P->IsActorBeingDestroyed()) Missiles.Add(P.Get());
		}
	}
	N = Missiles.Num();
	Ar << N;
	for (int32 i = 0; i < N; ++i)
	{
		FString Type;
		FVector Where = FVector::ZeroVector, Dir = FVector::ForwardVector;
		int32 SourceCode = 0, TracerCode = 0;
		if (Ar.IsSaving())
		{
			ADoomProjectile* P = Missiles[i];
			Type = P->Def->Name;
			Where = P->GetActorLocation();
			Dir = P->Movement->Velocity.GetSafeNormal(); // the actor reports no velocity before its first tick
			SourceCode = ActorCode(P->Source.Get());
			TracerCode = ActorCode(P->Tracer.Get());
		}
		Ar << Type << Where << Dir << SourceCode << TracerCode;
		if (Ar.IsLoading())
		{
			ADoomProjectile* P = SpawnProjectile(Type, CodeActor(SourceCode), Where, Dir, CodeActor(TracerCode), true);
			if (!P)
			{
				Ar.SetError();
				return;
			}
			Missiles.Add(P);
		}
		Missiles[i]->DoomSerialize(Ar);
	}

	if (Ar.IsLoading())
	{
		Flow().TotalKills = SavedTotalKills;
		Flow().TotalItems = SavedTotalItems;
	}
}

bool UDoomWorld::SaveGame(int32 Slot, const FString& Name)
{
	ADoomPlayer* P = GetPlayer();
	if (!Map.IsValid() || !P || P->IsDead() || (Flow().Phase != EDoomGameState::Playing && Flow().Phase != EDoomGameState::Menu))
	{
		ShowMessage(TEXT("You can't save now."));
		return false;
	}
	UDoomSaveGame* Save = Cast<UDoomSaveGame>(UGameplayStatics::CreateSaveGameObject(UDoomSaveGame::StaticClass()));
	Save->IWad = IWadName;
	Save->Map = Flow().CurrentMap;
	Save->Skill = Skill;
	Save->Saved = FDateTime::Now();
	const int32 Secs = Flow().LevelTic / Doom::TicRate;
	Save->Description = !Name.TrimStartAndEnd().IsEmpty() ? Name.TrimStartAndEnd()
		: FString::Printf(TEXT("%s %d:%02d K%d/%d"), *Flow().CurrentMap, Secs / 60, Secs % 60, Flow().Kills, Flow().TotalKills);
	FMemoryWriter Writer(Save->State, true);
	SerializeState(Writer);
	const bool bOk = !Writer.IsError() && UGameplayStatics::SaveGameToSlot(Save, SlotName(Slot), 0);
	SlotCache.Remove(Slot);
	ShowMessage(bOk ? (Slot <= 0 ? TEXT("Quicksaved.") : FString::Printf(TEXT("Game saved in slot %d."), Slot)) : TEXT("Save failed."));
	UE_LOG(LogDoom, Log, TEXT("Save %s: %s (%d bytes)"), *SlotName(Slot), bOk ? TEXT("ok") : TEXT("FAILED"), Save->State.Num());
	return bOk;
}

bool UDoomWorld::LoadGame(int32 Slot)
{
	const FString Name = SlotName(Slot);
	UDoomSaveGame* Save = UGameplayStatics::DoesSaveGameExist(Name, 0) ? Cast<UDoomSaveGame>(UGameplayStatics::LoadGameFromSlot(Name, 0)) : nullptr;
	if (!Save)
	{
		ShowMessage(TEXT("Nothing saved there."));
		return false;
	}
	if (Save->Version != UDoomSaveGame::CurrentVersion || (!Save->IWad.IsEmpty() && Save->IWad != IWadName))
	{
		ShowMessage(FString::Printf(TEXT("That save is for %s."), *Save->IWad));
		return false;
	}
	Skill = Save->Skill;
	if (!LoadMap(Save->Map, false, true))
	{
		return false;
	}
	FMemoryReader Reader(Save->State, true);
	SerializeState(Reader);
	if (Reader.IsError())
	{
		UE_LOG(LogDoom, Error, TEXT("Save %s is damaged; restarting the map"), *Name);
		LoadMap(Save->Map, true);
		return false;
	}
	// geometry and lights follow the restored sectors
	for (int32 s = 0; s < Map->Sectors.Num(); ++s) Level->SectorChanged(s);
	for (int32 l = 0; l < Map->Lines.Num(); ++l) Level->LineChanged(l);
	Level->Flush();
	Level->UpdateSectorLights();
	for (const TWeakObjectPtr<ADoomThing>& T : Things)
	{
		if (T.IsValid()) T->SnapToPlanes();
	}
	ResetClock();
	Flow().Phase = EDoomGameState::Playing;
	ShowMessage(Slot <= 0 ? TEXT("Quickloaded.") : FString::Printf(TEXT("Loaded slot %d."), Slot));
	UE_LOG(LogDoom, Log, TEXT("Loaded %s: %s, %d monsters, %d things, %d movers"), *Name, *Save->Description, Monsters.Num(), Things.Num(), Movers.Num());
	return true;
}

void UDoomWorld::RefreshSlots()
{
	SlotCache.Reset();
}

FString UDoomWorld::SlotDescription(int32 Slot)
{
	if (const FString* Cached = SlotCache.Find(Slot))
	{
		return *Cached;
	}
	FString Desc = TEXT("EMPTY");
	const FString Name = SlotName(Slot);
	if (UGameplayStatics::DoesSaveGameExist(Name, 0))
	{
		if (UDoomSaveGame* Save = Cast<UDoomSaveGame>(UGameplayStatics::LoadGameFromSlot(Name, 0)))
		{
			Desc = Save->IWad == IWadName || Save->IWad.IsEmpty() ? Save->Description : FString::Printf(TEXT("(%s)"), *Save->IWad);
		}
	}
	SlotCache.Add(Slot, Desc);
	return Desc;
}

FString UDoomWorld::StateFingerprint() const
{
	if (!Map.IsValid())
	{
		return TEXT("-");
	}
	double Planes = 0;
	int32 Light = 0;
	for (const FDoomSector& S : Map->Sectors)
	{
		Planes += S.Floor * 3 + S.Ceiling;
		Light += S.Light;
	}
	int32 Alive = 0, Health = 0, Dead = 0;
	FVector MonsterSum = FVector::ZeroVector;
	for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
	{
		if (!M.IsValid()) continue;
		if (M->IsDead()) ++Dead; else { ++Alive; Health += M->Health; }
		MonsterSum += FeetOf(M.Get());
	}
	int32 NumThings = 0;
	for (const TWeakObjectPtr<ADoomThing>& T : Things)
	{
		if (T.IsValid() && !T->bEffect) ++NumThings;
	}
	int32 NumMissiles = 0;
	FVector MissileSum = FVector::ZeroVector, MissileVel = FVector::ZeroVector;
	for (const TWeakObjectPtr<ADoomProjectile>& P : Projectiles)
	{
		if (!P.IsValid() || P->bExploded) continue;
		++NumMissiles;
		MissileSum += P->GetActorLocation();
		MissileVel += P->Movement->Velocity;
	}
	FString Player = TEXT("none");
	if (const ADoomPlayer* P = GetPlayer())
	{
		const FVector F = FeetOf(P);
		Player = FString::Printf(TEXT("(%.0f,%.0f,%.0f) hp%d ar%d am%d/%d/%d/%d w%d"), F.X, F.Y, F.Z, P->Inv().Health, P->Inv().Armor,
			P->Inv().Ammo[0], P->Inv().Ammo[1], P->Inv().Ammo[2], P->Inv().Ammo[3], (int32)P->Inv().Ready);
	}
	return FString::Printf(TEXT("tic%d planes%.1f light%d movers%d monsters%d/%d hp%d at(%.0f,%.0f,%.0f) things%d kills%d/%d items%d/%d player%s missiles%d at(%.0f,%.0f,%.0f) v(%.0f,%.0f,%.0f)"),
		Flow().LevelTic, Planes, Light, Movers.Num(), Alive, Dead, Health, MonsterSum.X, MonsterSum.Y, MonsterSum.Z, NumThings,
		Flow().Kills, Flow().TotalKills, Flow().Items, Flow().TotalItems, *Player, NumMissiles, MissileSum.X, MissileSum.Y, MissileSum.Z, MissileVel.X, MissileVel.Y, MissileVel.Z);
}

void UDoomWorld::RunSaveTest()
{
	// Something in motion: open the first manual door and let a few tics run first.
	for (int32 l = 0; l < Map->Lines.Num(); ++l)
	{
		const FDoomLine& L = Map->Lines[l];
		if (L.Special == 1 && L.BackSector != INDEX_NONE && !SectorHasMover(L.BackSector))
		{
			AddDoor(L.BackSector, 0, 2.f, nullptr);
			break;
		}
	}
	// corpses and awake monsters with targets
	int32 Index = 0;
	for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
	{
		if (!M.IsValid()) continue;
		if (Index < 2) UGameplayStatics::ApplyDamage(M.Get(), 10000.f, nullptr, GetPlayer(), nullptr);
		else if (Index < 5) M->WakeUp(GetPlayer(), false);
		++Index;
	}
	for (int32 i = 0; i < 20; ++i) RunTic();
	if (ADoomPlayer* P = GetPlayer())
	{
		P->Inv().Ammo[1] = 7; // make the inventory distinguishable from a fresh start
		P->Inv().Weapons[(int32)EDoomWeapon::Shotgun] = true;
		// missiles in flight: a rocket from the player, and a homing tracer from an awake monster
		const FVector Eye = P->GetActorLocation() + FVector(0, 0, 20.f * Doom::Scale);
		SpawnProjectile(TEXT("Rocket"), P, Eye, P->GetActorForwardVector(), nullptr, true);
		for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
		{
			if (M.IsValid() && !M->IsDead() && M->Target.IsValid())
			{
				SpawnProjectile(TEXT("Tracer"), M.Get(), M->GetActorLocation(), (Eye - M->GetActorLocation()).GetSafeNormal(), P, true);
				break;
			}
		}
		for (int32 i = 0; i < 3; ++i) RunTic();
	}
	int32 Awake = 0;
	for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
	{
		if (M.IsValid() && M->Target.IsValid()) ++Awake;
	}
	const FString Before = StateFingerprint() + FString::Printf(TEXT(" targets%d"), Awake);
	const bool bSaved = SaveGame(9);

	// scramble: run on, move the player, hurt the monsters, change sectors
	for (int32 i = 0; i < 70; ++i) RunTic();
	if (ADoomPlayer* P = GetPlayer())
	{
		P->SetActorLocation(P->GetActorLocation() + FVector(300, 0, 0));
		P->Inv().Ammo[1] = 0;
	}
	for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
	{
		if (M.IsValid() && !M->IsDead()) M->Health = 1;
	}
	for (FDoomSector& S : Map->Sectors) S.Light = 0;
	const FString Scrambled = StateFingerprint() + TEXT(" targets?");

	const bool bLoaded = LoadGame(9);
	int32 AwakeAfter = 0;
	for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
	{
		if (M.IsValid() && M->Target.IsValid()) ++AwakeAfter;
	}
	const FString After = StateFingerprint() + FString::Printf(TEXT(" targets%d"), AwakeAfter);
	const bool bPass = bSaved && bLoaded && Before == After && Before != Scrambled;
	UE_LOG(LogDoom, Display, TEXT("Savetest %s\n  before:    %s\n  scrambled: %s\n  after:     %s"),
		bPass ? TEXT("PASS") : TEXT("FAIL"), *Before, *Scrambled, *After);
}

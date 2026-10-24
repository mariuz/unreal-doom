// UDoomWorld: level flow and patches, generated maps' options, respawning and boss deaths, credit,
// players and their starts, the network (replicated flow, sectors and sides, client tick, the net
// test) and the game state.
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

DoomMapGen::FOptions UDoomWorld::GenOptions(uint32 Seed) const
{
	return DoomMapGen::DefaultOptions(Seed, Wad.IsValid() && Wad->IsDoom2());
}

FString UDoomWorld::MusicForMap(const FString& MapName) const
{
	uint32 GenSeed = 0;
	if (DoomMapGen::ParseName(MapName, GenSeed) && Wad.IsValid())
	{
		// one of the IWAD's own songs, by seed
		const bool bDoom2 = Wad->IsDoom2();
		return DoomMusic::LumpForMap(bDoom2 ? FString::Printf(TEXT("MAP%02d"), 1 + GenSeed % 30)
			: FString::Printf(TEXT("E%dM%d"), 1 + (GenSeed / 9) % 3, 1 + GenSeed % 9));
	}
	return DoomMusic::LumpForMap(MapName);
}

FString UDoomWorld::LevelTitle(const FString& MapName) const
{
	if (const FDoomMapInfo* Info = MapInfo(MapName))
	{
		if (!Info->LevelName.IsEmpty())
		{
			if (Info->bClearLabel) return Info->LevelName;
			return FString::Printf(TEXT("%s: %s"), Info->Label.IsEmpty() ? *MapName : *Info->Label, *Info->LevelName);
		}
	}
	uint32 GenSeed = 0;
	if (DoomMapGen::ParseName(MapName, GenSeed))
	{
		return FString::Printf(TEXT("RANDOM MAP %05X"), GenSeed);
	}
	// the WAD's own level names (DEHACKED / BEX strings): HUSTR_E1M1 or HUSTR_1 for MAP01
	const FString Key = MapName.StartsWith(TEXT("MAP")) ? FString::Printf(TEXT("HUSTR_%d"), FCString::Atoi(*MapName.Mid(3))) : TEXT("HUSTR_") + MapName;
	return DoomInfo::Text(*Key, MapName);
}

void UDoomWorld::LoadPatches(const TArray<FString>& DehFiles)
{
	DoomInfo::ResetTables();
	MapInfos.Reset();
	// every DEHACKED / UMAPINFO lump in load order: the IWAD's first, PWADs patch on top
	for (int32 i = 0; i < Wad->NumLumps(); ++i)
	{
		const FString& LumpName = Wad->Lump(i).NameStr;
		if (LumpName != TEXT("DEHACKED") && LumpName != TEXT("UMAPINFO"))
		{
			continue;
		}
		TConstArrayView<uint8> D = Wad->LumpData(i);
		const FString Text(D.Num(), reinterpret_cast<const ANSICHAR*>(D.GetData()));
		if (LumpName == TEXT("DEHACKED"))
		{
			UE_LOG(LogDoomGame, Log, TEXT("DEHACKED lump: %s"), *DoomDehacked::Apply(Text).ToString());
		}
		else
		{
			UE_LOG(LogDoomGame, Log, TEXT("UMAPINFO lump: %d maps"), MapInfos.Parse(Text));
		}
	}
	for (const FString& File : DehFiles)
	{
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *File))
		{
			UE_LOG(LogDoomGame, Log, TEXT("%s: %s"), *FPaths::GetCleanFilename(File), *DoomDehacked::Apply(Text).ToString());
		}
		else
		{
			UE_LOG(LogDoomGame, Error, TEXT("Cannot read DeHackEd patch %s"), *File);
		}
	}
	// UMAPINFO is the more specific source: its par times win over DeHackEd's [PARS]
	for (const TPair<FString, FDoomMapInfo>& Pair : MapInfos.All())
	{
		if (Pair.Value.ParTime >= 0)
		{
			DoomInfo::SetParTime(Pair.Key, Pair.Value.ParTime);
		}
	}
}

void UDoomWorld::TicRespawn()
{
	// Nightmare (or -respawn): a corpse dead 12 s may rise at its spawn spot, checked every 32 tics
	static const bool bRespawnParam = FParse::Param(FCommandLine::Get(), TEXT("respawn"));
	if ((Skill < 5 && !bRespawnParam) || (Flow().LevelTic & 31) != 0)
	{
		return;
	}
	const TArray<TWeakObjectPtr<ADoomMonster>> Current = Monsters;
	for (const TWeakObjectPtr<ADoomMonster>& Weak : Current)
	{
		ADoomMonster* M = Weak.Get();
		if (!M || M->State != EMonsterState::Dead || !M->bCanRespawn || M->DeadTics < 12 * Doom::TicRate || DoomInfo::Random() > 4)
		{
			continue;
		}
		// the spot must be free of bodies
		const float S = Doom::Scale;
		const FVector Center = M->SpawnFeet + FVector(0, 0, M->Def->Height * 0.5f * S + 1.f);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomRespawn), false, M);
		FCollisionObjectQueryParams Bodies;
		Bodies.AddObjectTypesToQuery(ECC_Pawn);
		Bodies.AddObjectTypesToQuery(ECC_DoomMonster);
		if (GetWorld()->OverlapAnyTestByObjectType(Center, FQuat::Identity, Bodies,
			FCollisionShape::MakeCapsule(M->Def->Radius * S, M->Def->Height * 0.5f * S), Q))
		{
			continue;
		}
		const FVector Old = M->GetActorLocation();
		SpawnEffect(TEXT("TFOG"), TEXT("ABABCDEFGHIJ"), 6, Old);
		SpawnEffect(TEXT("TFOG"), TEXT("ABABCDEFGHIJ"), 6, Center);
		PlaySound(TEXT("TELEPT"), &Center);
		if (ADoomMonster* Risen = SpawnMonster(M->Def->DoomEdNum, M->SpawnFeet, M->SpawnAngle))
		{
			--Flow().TotalKills; // like Doom, the kill already counted stays counted
			Risen->SpawnFeet = M->SpawnFeet;
			Risen->SpawnAngle = M->SpawnAngle;
			Risen->bCanRespawn = true;
			UE_LOG(LogDoomGame, Log, TEXT("Nightmare: %s respawned"), *Risen->Def->Name);
		}
		M->Destroy();
	}
}

void UDoomWorld::OnMonsterDied(ADoomMonster* Dead)
{
	if (!Dead || !Dead->Def)
	{
		return;
	}
	// UMAPINFO boss actions replace the vanilla boss specials for their map
	if (const FDoomMapInfo* Info = MapInfo(Flow().CurrentMap))
	{
		if (Info->bClearBossActions)
		{
			for (const FDoomBossAction& A : Info->BossActions)
			{
				if (A.DoomEdNum != Dead->Def->DoomEdNum)
				{
					continue;
				}
				const bool bAllDead = !Monsters.ContainsByPredicate([&](const TWeakObjectPtr<ADoomMonster>& M)
				{
					return M.IsValid() && M->Def->DoomEdNum == A.DoomEdNum && !M->IsDead();
				});
				if (bAllDead)
				{
					UE_LOG(LogDoomGame, Log, TEXT("Boss action: special %d on tag %d"), A.Special, A.Tag);
					ExecuteSpecial(A.Special, A.Tag);
				}
			}
			return;
		}
	}
	auto AllDead = [&](const TCHAR* Name)
	{
		for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
		{
			if (M.IsValid() && M->Def->Name.Equals(Name, ESearchCase::CaseSensitive) && !M->IsDead()) return false;
		}
		return true;
	};
	const FString Type = Dead->Def->Name;
	auto Tagged = [&](int32 Tag, TFunctionRef<void(int32)> Fn)
	{
		for (int32 S : Map->SectorsWithTag(Tag)) if (!SectorHasMover(S)) Fn(S);
	};

	if (Type == TEXT("CommanderKeen") && AllDead(TEXT("CommanderKeen")))
	{
		Tagged(666, [&](int32 S) { AddDoor(S, DoomWorldDetail::DK_Open, DoomWorldDetail::DoorSpeed, nullptr); });
		return;
	}
	if (Type == TEXT("BossBrain"))
	{
		// explosions around the brain for three seconds, then the level ends
		BrainExitTics = 3 * Doom::TicRate;
		PlaySound(TEXT("BOSDTH"), nullptr);
		return;
	}
	if (Flow().CurrentMap == TEXT("MAP07"))
	{
		if (Type == TEXT("Mancubus") && AllDead(TEXT("Mancubus"))) Tagged(666, [&](int32 S) { AddFloor(S, DoomWorldDetail::FK_LowerLowest, 1, -1); });
		if (Type == TEXT("Arachnotron") && AllDead(TEXT("Arachnotron"))) Tagged(667, [&](int32 S) { AddFloor(S, DoomWorldDetail::FK_RaiseShortest, 1, -1); });
		return;
	}
	if (Flow().CurrentMap == TEXT("E1M8") && Type == TEXT("BaronOfHell") && AllDead(TEXT("BaronOfHell")))
	{
		Tagged(666, [&](int32 S) { AddFloor(S, DoomWorldDetail::FK_LowerLowest, 1, -1); });
	}
	else if ((Flow().CurrentMap == TEXT("E2M8") && Type == TEXT("Cyberdemon")) || (Flow().CurrentMap == TEXT("E3M8") && Type == TEXT("SpiderMastermind")))
	{
		if (AllDead(*Type)) ExitLevel(false);
	}
	else if (Flow().CurrentMap == TEXT("E4M6") && Type == TEXT("Cyberdemon") && AllDead(TEXT("Cyberdemon")))
	{
		Tagged(666, [&](int32 S) { AddDoor(S, DoomWorldDetail::DK_Open, DoomWorldDetail::DoorSpeed * 4, nullptr); });
	}
	else if (Flow().CurrentMap == TEXT("E4M8") && Type == TEXT("SpiderMastermind") && AllDead(TEXT("SpiderMastermind")))
	{
		Tagged(666, [&](int32 S) { AddFloor(S, DoomWorldDetail::FK_LowerLowest, 1, -1); });
	}
}

// ------------------------------------------------------------------------------------------------
// Level flow
// ------------------------------------------------------------------------------------------------

// The level flow belongs to the game mode (ADoomGameMode::ExitLevel and friends, DoomGameMode.cpp).
FString UDoomWorld::NextMapName(bool bSecret) const
{
	const ADoomGameMode* GM = GetWorld()->GetAuthGameMode<ADoomGameMode>();
	return GM ? GM->NextMapName(bSecret) : FString();
}

void UDoomWorld::ExitLevel(bool bSecret)
{
	if (ADoomGameMode* GM = GetWorld()->GetAuthGameMode<ADoomGameMode>())
	{
		GM->ExitLevel(bSecret);
	}
}

void UDoomWorld::FinishIntermission()
{
	if (ADoomGameMode* GM = GetWorld()->GetAuthGameMode<ADoomGameMode>())
	{
		GM->FinishIntermission();
	}
}

// ------------------------------------------------------------------------------------------------
// Gameplay framework glue: flow record, credit, clock
// ------------------------------------------------------------------------------------------------

ADoomGameState* UDoomWorld::GetDoomGameState() const
{
	return GetWorld() ? GetWorld()->GetGameState<ADoomGameState>() : nullptr;
}

FDoomLevelFlow& UDoomWorld::Flow()
{
	ADoomGameState* GS = GetDoomGameState();
	return GS ? GS->Flow : LocalFlow;
}

const FDoomLevelFlow& UDoomWorld::Flow() const
{
	const ADoomGameState* GS = GetDoomGameState();
	return GS ? GS->Flow : LocalFlow;
}

/** The player behind a kill: the shooter of a missile, the player, or their controller
    (damage reports a pawn's owner, which for players is the PlayerController). */
ADoomPlayerState* UDoomWorld::CreditedPlayer(AActor* Actor)
{
	if (const ADoomProjectile* Missile = Cast<ADoomProjectile>(Actor))
	{
		Actor = Missile->Source.Get();
	}
	if (const AController* C = Cast<AController>(Actor))
	{
		return C->GetPlayerState<ADoomPlayerState>();
	}
	const ADoomPlayer* P = Cast<ADoomPlayer>(Actor);
	return P ? P->GetDoomPlayerState() : nullptr;
}

void UDoomWorld::CreditKill(AActor* Killer)
{
	++Flow().Kills;
	if (ADoomPlayerState* PS = CreditedPlayer(Killer))
	{
		++PS->LevelKills;
	}
}

void UDoomWorld::CreditItem(ADoomPlayer* Player)
{
	++Flow().Items;
	if (ADoomPlayerState* PS = CreditedPlayer(Player))
	{
		++PS->LevelItems;
	}
}

void UDoomWorld::CreditSecret(ADoomPlayer* Player)
{
	++Flow().Secrets;
	if (ADoomPlayerState* PS = CreditedPlayer(Player))
	{
		++PS->LevelSecrets;
	}
}

void UDoomWorld::ResetClock()
{
	if (ADoomGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<ADoomGameMode>() : nullptr)
	{
		GM->ResetClock();
	}
}


// ------------------------------------------------------------------------------------------------
// Networking and players
// ------------------------------------------------------------------------------------------------

bool UDoomWorld::IsNetClient() const
{
	return GetWorld() && GetWorld()->GetNetMode() == NM_Client;
}

bool UDoomWorld::IsNetServer() const
{
	const ENetMode M = GetWorld() ? GetWorld()->GetNetMode() : NM_Standalone;
	return M == NM_ListenServer || M == NM_DedicatedServer;
}

bool UDoomWorld::IsNetGame() const
{
	return GetWorld() && GetWorld()->GetNetMode() != NM_Standalone;
}

TArray<ADoomPlayer*> UDoomWorld::GetPlayers() const
{
	TArray<ADoomPlayer*> Out;
	if (UWorld* W = GetWorld())
	{
		for (TActorIterator<ADoomPlayer> It(W); It; ++It)
		{
			if (!It->IsActorBeingDestroyed()) Out.Add(*It);
		}
	}
	return Out;
}

ADoomPlayer* UDoomWorld::NearestPlayer(const AActor* From, bool bInSight) const
{
	ADoomPlayer* Best = nullptr;
	double BestDist = TNumericLimits<double>::Max();
	for (ADoomPlayer* P : GetPlayers())
	{
		if (P->IsDead() || (bInSight && !CheckSight(From, P)))
		{
			continue;
		}
		const double D = FVector::DistSquared(P->GetActorLocation(), From->GetActorLocation());
		if (D < BestDist)
		{
			BestDist = D;
			Best = P;
		}
	}
	return Best;
}

void UDoomWorld::PlacePlayer(ADoomPlayer* P)
{
	if (!P || !Map.IsValid())
	{
		return;
	}
	const ADoomPlayerState* PS = P->GetDoomPlayerState();
	const int32 Number = PS ? FMath::Clamp(PS->PlayerNumber, 0, 3) : 0;
	FDoomMapThing Start;
	FVector2f Offset = FVector2f::ZeroVector;
	if (IsDeathmatch() && DeathmatchStarts.Num() > 0)
	{
		Start = DeathmatchStarts[FMath::RandRange(0, DeathmatchStarts.Num() - 1)];
	}
	else if (PlayerStarts.IsValidIndex(Number) && PlayerStarts[Number].Type != 0)
	{
		Start = PlayerStarts[Number];
	}
	else if (PlayerStarts.Num() > 0 && PlayerStarts[0].Type != 0)
	{
		// a single-player map: line up beside player 1's start
		Start = PlayerStarts[0];
		Offset = FVector2f(0, 40.f * Number);
	}
	else
	{
		return;
	}
	const FVector2f At = Start.P + Offset;
	const int32 Sec = Map->SectorAt(At);
	const float Floor = Sec >= 0 ? Map->Sectors[Sec].Floor : 0;
	P->SetActorLocation(Doom::ToUE(At, Floor) + FVector(0, 0, Doom::HalfHeight(P) + 2), false, nullptr, ETeleportType::TeleportPhysics);
	const FRotator Facing(0, Doom::AngleToYaw(Start.Angle), 0);
	if (AController* C = P->GetController())
	{
		C->SetControlRotation(Facing);
		if (APlayerController* PC = Cast<APlayerController>(C))
		{
			PC->ClientSetRotation(Facing); // a remote player's view turns on their machine
		}
	}
}

void UDoomWorld::OnFlowReplicated()
{
	if (!IsNetClient() || !EnsureWads())
	{
		return;
	}
	const FDoomLevelFlow& F = Flow();
	if (!F.CurrentMap.IsEmpty() && F.LevelSerial != LoadedSerial && F.Phase == EDoomGameState::Playing)
	{
		UE_LOG(LogDoomNet, Log, TEXT("Client: building %s (level %d) from the server"), *F.CurrentMap, F.LevelSerial);
		LoadMap(F.CurrentMap, false, true);
		// changes the server published before this map was built
		if (const ADoomGameState* GS = GetDoomGameState())
		{
			for (const FDoomSectorNet& N : GS->SectorNet.Items) ApplySectorNet(N);
			for (const FDoomSideNet& N : GS->SideNet.Items) ApplySideNet(N);
		}
	}
	if (F.Phase != LastClientPhase)
	{
		if (F.Phase == EDoomGameState::Intermission)
		{
			const FDoomMapInfo* Info = MapInfo(F.CurrentMap);
			PlayMusic((Info && !Info->InterMusic.IsEmpty()) ? Info->InterMusic : Wad->IsDoom2() ? TEXT("D_DM2INT") : TEXT("D_INTER"));
		}
		LastClientPhase = F.Phase;
	}
}

void UDoomWorld::ApplySectorNet(const FDoomSectorNet& N)
{
	if (!IsNetClient() || !Map.IsValid() || !Level || N.LevelSerial != LoadedSerial || !Map->Sectors.IsValidIndex(N.Sector))
	{
		return; // not built yet: OnFlowReplicated applies the whole array after building
	}
	FDoomSector& S = Map->Sectors[N.Sector];
	const bool bGeometry = S.Floor != N.Floor || S.Ceiling != N.Ceiling || S.FloorPic != N.FloorPic || S.CeilingPic != N.CeilingPic;
	S.Floor = N.Floor;
	S.Ceiling = N.Ceiling;
	S.Light = N.Light;
	S.Special = N.Special;
	S.FloorPic = N.FloorPic;
	S.CeilingPic = N.CeilingPic;
	if (bGeometry)
	{
		Level->SectorChanged(N.Sector);
	}
	bClientGeometryDirty = true;
}

void UDoomWorld::ApplySideNet(const FDoomSideNet& N)
{
	if (!IsNetClient() || !Map.IsValid() || !Level || N.LevelSerial != LoadedSerial || !Map->Sides.IsValidIndex(N.Side))
	{
		return;
	}
	FDoomSide& S = Map->Sides[N.Side];
	S.Upper = N.Upper;
	S.Middle = N.Middle;
	S.Lower = N.Lower;
	if (S.Line != INDEX_NONE)
	{
		Level->LineChanged(S.Line);
	}
	bClientGeometryDirty = true;
}

void UDoomWorld::AddSleepingSprite(UDoomSpriteComponent* Sprite)
{
	SleepingSprites.Add(Sprite);
}

void UDoomWorld::Tick(float DeltaTime)
{
	if (IsNetClient())
	{
		ClientTick(DeltaTime);
	}
	// sleeping corpse sprites: only the ones on screen need turning to the camera (off screen, their
	// rotation catches up the first frame they show)
	const APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const FVector View = Cam ? Cam->GetCameraLocation() : FVector::ZeroVector;
	for (int32 i = SleepingSprites.Num() - 1; i >= 0; --i)
	{
		UDoomSpriteComponent* S = SleepingSprites[i].Get();
		if (!S || !S->IsSleeping())
		{
			SleepingSprites.RemoveAtSwap(i, EAllowShrinking::No); // gone, or awake and ticking again
			continue;
		}
		if (Cam && S->WasRecentlyRendered(0.2f))
		{
			S->RefreshFacing(View);
		}
	}
}

void UDoomWorld::ClientTick(float DeltaTime)
{
	if (!Map.IsValid() || !Level)
	{
		return;
	}
	if (bClientGeometryDirty)
	{
		bClientGeometryDirty = false;
		Level->Flush();
		Level->UpdateSectorLights();
		for (const TWeakObjectPtr<ADoomThing>& T : Things)
		{
			if (T.IsValid()) T->SnapToPlanes();
		}
	}
	ClientAccumulator += FMath::Min(DeltaTime, 0.25f);
	while (ClientAccumulator >= Doom::TicSeconds)
	{
		ClientAccumulator -= Doom::TicSeconds;
		++ClientTic;
		if (MessageTics > 0) --MessageTics;
		Level->AnimateTextures(ClientTic);
	}
	UpdateMaterialGlobals(ClientAccumulator / Doom::TicSeconds);
}

int32 UDoomWorld::NetTestDoorSector() const
{
	if (Map.IsValid())
	{
		for (const FDoomLine& L : Map->Lines)
		{
			if (L.Special == 1 && L.BackSector != INDEX_NONE) return L.BackSector;
		}
	}
	return INDEX_NONE;
}

void UDoomWorld::NetTestOpenDoor()
{
	const int32 Sec = NetTestDoorSector();
	if (Sec != INDEX_NONE && !SectorHasMover(Sec))
	{
		AddDoor(Sec, 0, 2.f, nullptr);
	}
}

FString UDoomWorld::NetFingerprint() const
{
	if (!Map.IsValid())
	{
		return TEXT("no map");
	}
	int32 Alive = 0, Dead = 0, NumThings = 0;
	for (TActorIterator<ADoomMonster> It(GetWorld()); It; ++It)
	{
		if (It->bNetCorpse || It->IsDead()) ++Dead; else ++Alive;
	}
	for (TActorIterator<ADoomThing> It(GetWorld()); It; ++It)
	{
		if (!It->bEffect && It->Def) ++NumThings;
	}
	const int32 Door = NetTestDoorSector();
	FString Players;
	if (const ADoomGameState* GS = GetDoomGameState())
	{
		for (const ADoomPlayerState* PS : GS->DoomPlayers())
		{
			// the marine's colour as this machine draws it (Doom's translation: 0 green .. 3 red)
			const ADoomPlayer* Pawn = PS->GetPawn<ADoomPlayer>();
			Players += FString::Printf(TEXT(" [P%d hp%d am%d w%d k%d f%d c%d]"), PS->PlayerNumber + 1, PS->Inv.Health, PS->Inv.Ammo[0],
				(int32)PS->Inv.Weapons[(int32)EDoomWeapon::Shotgun], PS->LevelKills, PS->Frags, Pawn && Pawn->Body ? Pawn->Body->Translation : -1);
		}
		if (Flow().Phase == EDoomGameState::Finale)
		{
			Players += FString::Printf(TEXT(" finale stage%d tic%d lines%d"), (int32)FinaleStage, FinaleTic, FinaleText.Num());
		}
	}
	return FString::Printf(TEXT("%s level%d phase%d monsters%d/%d things%d door%d ceiling%.0f kills%d/%d%s"),
		*Flow().CurrentMap, Flow().LevelSerial, (int32)Flow().Phase, Alive, Dead, NumThings, Door,
		Door != INDEX_NONE ? Map->Sectors[Door].Ceiling : 0.f, Flow().Kills, Flow().TotalKills, *Players);
}

void UDoomWorld::SetState(EDoomGameState S)
{
	if (S != EDoomGameState::Playing)
	{
		// the menu swaps the input mappings, and a held trigger or run key may never see its release:
		// don't come back auto-firing
		for (ADoomPlayer* P : GetPlayers())
		{
			if (P->IsLocallyControlled())
			{
				P->bFireHeld = P->bRunHeld = false;
			}
		}
	}
	if (IsNetGame())
	{
		// network games never pause: the menu is this player's overlay, not a phase of the game
		if (S == EDoomGameState::Menu)
		{
			bNetMenu = true;
			return;
		}
		if (bNetMenu && (S == EDoomGameState::Playing || S == EDoomGameState::Title))
		{
			bNetMenu = false;
			return;
		}
		bNetMenu = false;
		if (IsNetClient())
		{
			return; // the server decides the phase
		}
	}
	Flow().Phase = S;
}

void UDoomWorld::ClassifySectors(const FDoomMap& InMap, TArray<ESectorRole>& Out)
{
	Out.Init(ESectorRole::None, InMap.Sectors.Num());
	for (const FDoomLine& L : InMap.Lines)
	{
		const DoomWorldDetail::FSpec* S = L.Special ? DoomWorldDetail::FindSpec(L.Special) : nullptr;
		if (!S)
		{
			continue;
		}
		ESectorRole Role = ESectorRole::None;
		switch (S->Act)
		{
		case DoomWorldDetail::EAct::DoorRaise: case DoomWorldDetail::EAct::DoorOpen: case DoomWorldDetail::EAct::DoorBlazeRaise: case DoomWorldDetail::EAct::DoorBlazeOpen: case DoomWorldDetail::EAct::DoorClose30:
			Role = ESectorRole::Door; break;
		case DoomWorldDetail::EAct::Lift: case DoomWorldDetail::EAct::LiftBlaze: case DoomWorldDetail::EAct::PlatPerpetual:
			Role = ESectorRole::Lift; break;
		default: break;
		}
		if (Role == ESectorRole::None)
		{
			continue;
		}
		if (S->Trig == 'D')
		{
			if (L.BackSector != INDEX_NONE) Out[L.BackSector] = Role;
		}
		else
		{
			for (int32 Sec : InMap.SectorsWithTag(L.Tag)) Out[Sec] = Role;
		}
	}
}

void UDoomWorld::FindTeleporters(const FDoomMap& InMap, TArray<TPair<int32, int32>>& Out)
{
	for (int32 l = 0; l < InMap.Lines.Num(); ++l)
	{
		const FDoomLine& L = InMap.Lines[l];
		const DoomWorldDetail::FSpec* S = L.Special ? DoomWorldDetail::FindSpec(L.Special) : nullptr;
		if (!S || S->Act != DoomWorldDetail::EAct::Teleport)
		{
			continue;
		}
		for (int32 Sec : InMap.SectorsWithTag(L.Tag))
		{
			for (int32 t = 0; t < InMap.Things.Num(); ++t)
			{
				if (InMap.Things[t].Type == 14 && InMap.SectorAt(InMap.Things[t].P) == Sec)
				{
					Out.Add(TPair<int32, int32>(l, t));
				}
			}
		}
	}
}

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

UDoomWorld::UDoomWorld() = default;
UDoomWorld::~UDoomWorld() = default;

bool UDoomWorld::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UDoomWorld::Deinitialize()
{
	StopMusic();
	UnloadMap();
	Sound.Reset();
	Gfx.Reset();
	Wad.Reset();
	Super::Deinitialize();
}

bool UDoomWorld::InitWads(const FDoomWadSetup* Setup)
{
	// the level, things and sprites point into the old graphics: a loaded map cannot outlive them
	UnloadMap();
	if (Wad.IsValid())
	{
		SetState(EDoomGameState::Title);
	}
	const UDoomSettings* Settings = GetDefault<UDoomSettings>();
	auto Resolve = [](const FString& Name) -> FString
	{
		if (FPaths::FileExists(Name))
		{
			return Name;
		}
		const FString InContent = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("WADs"), Name);
		return InContent;
	};

	FString IWad = Settings->IWAD;
	if (Setup && !Setup->IWad.IsEmpty())
	{
		IWad = Setup->IWad;
	}
	else if (!Setup)
	{
		FParse::Value(FCommandLine::Get(), TEXT("-iwad="), IWad);
	}
	IWadName = FPaths::GetCleanFilename(IWad).ToLower();
	TArray<FString> PWads = Setup ? Setup->PWads : Settings->PWADs;
	FString FileArg;
	if (!Setup && FParse::Value(FCommandLine::Get(), TEXT("-file="), FileArg, false))
	{
		FileArg.ParseIntoArray(PWads, TEXT(","));
	}

	Wad = MakeUnique<FDoomWad>();
	if (!Wad->AddFile(Resolve(IWad)))
	{
		Wad.Reset();
		return false;
	}
	for (const FString& P : PWads)
	{
		Wad->AddFile(Resolve(P));
	}
	Wad->Finalize();
	WadSetName = UDoomMapTiles::WadSetName(IWad, PWads);
	if (!Wad->IsValid())
	{
		UE_LOG(LogDoom, Error, TEXT("WAD has no PLAYPAL"));
		Wad.Reset();
		return false;
	}
	FString DehArg;
	TArray<FString> DehFiles;
	if (Setup)
	{
		for (const FString& F : Setup->DehFiles) DehFiles.Add(Resolve(F));
	}
	else if (FParse::Value(FCommandLine::Get(), TEXT("-deh="), DehArg, false))
	{
		DehArg.ParseIntoArray(DehFiles, TEXT(","));
		for (FString& F : DehFiles) F = Resolve(F);
	}
	LoadPatches(DehFiles);
	Gfx = MakeUnique<FDoomGraphics>(*Wad);
	Sound = MakeUnique<FDoomSound>(*Wad);
	Sound->MasterVolume = Settings->SfxVolume;
	Skill = Settings->Skill;
	FParse::Value(FCommandLine::Get(), TEXT("-skill="), Skill);
	Skill = FMath::Clamp(Skill, 1, 5);
	bModern = Settings->bModernRenderer;
	if (FParse::Param(FCommandLine::Get(), TEXT("modern"))) bModern = true;
	if (FParse::Param(FCommandLine::Get(), TEXT("classic"))) bModern = false;
	bSmartAI = Settings->bSmartMonsters || FParse::Param(FCommandLine::Get(), TEXT("smartai"));
	if (FParse::Param(FCommandLine::Get(), TEXT("classicai"))) bSmartAI = false;
	bNoParticles = FParse::Param(FCommandLine::Get(), TEXT("noparticles"));
	bAltDeathParam = FParse::Param(FCommandLine::Get(), TEXT("altdeath"));
	bDeathmatchParam = FParse::Param(FCommandLine::Get(), TEXT("deathmatch")) || bAltDeathParam;
	bNoMonsters = FParse::Param(FCommandLine::Get(), TEXT("nomonsters"));
	bFastParam = FParse::Param(FCommandLine::Get(), TEXT("fast"));
	UE_LOG(LogDoom, Log, TEXT("%s: %d maps (%s)"), *IWad, Wad->MapNames().Num(), Wad->IsDoom2() ? TEXT("Doom 2") : TEXT("Doom 1"));
	return true;
}

ADoomPlayer* UDoomWorld::GetPlayer() const
{
	return Cast<ADoomPlayer>(UGameplayStatics::GetPlayerCharacter(GetWorld(), 0));
}

void UDoomWorld::StartNewGame(const FString& MapName, int32 InSkill)
{
	if (IsNetClient())
	{
		ShowMessage(TEXT("Only the server can start a new game."));
		return;
	}
	Skill = FMath::Clamp(InSkill, 1, 5);
	LoadMapAsync(MapName, true);
}

void UDoomWorld::SetModern(bool b)
{
	if (bModern != b)
	{
		bModern = b;
		if (!Map.IsValid())
		{
			return;
		}
		if (IsNetGame())
		{
			ShowMessage(TEXT("The renderer changes on the next map.")); // a reload would restart everyone's level
			return;
		}
		// rebuild with the other material set, keeping the game where it is (the menu is usually open):
		// the save game's state round trip, in memory
		TArray<uint8> State;
		FMemoryWriter Writer(State, true);
		SerializeState(Writer);
		const EDoomGameState Phase = Flow().Phase;
		if (LoadMap(Flow().CurrentMap, false, true))
		{
			FMemoryReader Reader(State, true);
			SerializeState(Reader);
			RefreshRestoredLevel();
			Flow().Phase = Phase;
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Map loading
// ------------------------------------------------------------------------------------------------

// A map load in steps (LoadMapAsync / StepLoad); LoadMap runs it without returning in between.
struct FDoomMapLoad
{
	enum class EStage : uint8 { Pictures, Geometry, Commit };
	EStage Stage = EStage::Pictures;
	bool bPistolStart = false;
	bool bForSaveGame = false;
	FName Sky;
	TSharedPtr<FDoomPicturePrefetch> Pictures;
	TArray<ADoomLevel::FChunkSections> Sections;
	UE::Tasks::FTask Task;
	int32 NextChunk = 0;
	double StartTime = 0, PicturesTime = 0, GeometryTime = 0;
};

void UDoomWorld::UnloadMap()
{
	if (PendingLoad.IsValid())
	{
		PendingLoad->Task.Wait(); // a worker may still be reading the map
		PendingLoad.Reset();
	}
	for (TWeakObjectPtr<ADoomThing>& T : Things) if (T.IsValid()) T->Destroy();
	for (TWeakObjectPtr<ADoomThing>& T : EffectPool) if (T.IsValid()) T->Destroy();
	EffectPool.Reset();
	for (TWeakObjectPtr<ADoomMonster>& M : Monsters) if (M.IsValid()) M->Destroy();
	for (TWeakObjectPtr<ADoomProjectile>& P : Projectiles) if (P.IsValid()) P->Destroy();
	for (AActor* A : EnvironmentActors) if (A) A->Destroy();
	Things.Reset();
	Monsters.Reset();
	Projectiles.Reset();
	ItemRespawns.Reset();
	EnvironmentActors.Reset();
	Movers.Reset();
	Lights.Reset();
	Buttons.Reset();
	for (FBrainCube& C : BrainCubes) if (C.Visual.IsValid()) C.Visual->Destroy();
	BrainCubes.Reset();
	BrainShooters.Reset();
	BrainTargets.Reset();
	bBrainAwake = false;
	BrainTimer = BrainTargetIndex = BrainExitTics = 0;
	if (Level)
	{
		Level->Destroy();
		Level = nullptr;
	}
	if (PostProcess)
	{
		PostProcess->Destroy();
		PostProcess = nullptr;
	}
	Map.Reset();
}

bool UDoomWorld::LoadMap(const FString& MapName, bool bPistolStart, bool bForSaveGame)
{
	if (!BeginLoad(MapName, bPistolStart, bForSaveGame))
	{
		return false;
	}
	StepLoad(MAX_dbl);
	return true;
}

bool UDoomWorld::LoadEditedMap(const FDoomMap& Edited)
{
	PendingEdited = MakeUnique<FDoomMap>(Edited);
	const bool bOk = LoadMap(Edited.Name, false, false);
	PendingEdited.Reset();
	return bOk;
}

bool UDoomWorld::LoadMapAsync(const FString& MapName, bool bPistolStart)
{
	return BeginLoad(MapName, bPistolStart, false);
}

bool UDoomWorld::BeginLoad(const FString& MapName, bool bPistolStart, bool bForSaveGame)
{
	const double StartTime = FPlatformTime::Seconds();
	if (!Wad.IsValid() && !InitWads())
	{
		return false;
	}
	UnloadMap();
	bNavBuilt = false;
	const bool bEdited = PendingEdited.IsValid();
	Map = bEdited ? MoveTemp(PendingEdited) : MakeUnique<FDoomMap>();
	uint32 GenSeed = 0;
	if (bEdited)
	{
		// the map editor's map, as it stands (LoadEditedMap)
	}
	else if (DoomMapGen::ParseName(MapName, GenSeed))
	{
		// a generated map: the same seed makes the same map (saves, network clients)
		if (!DoomMapGen::Generate(GenOptions(GenSeed), Gfx.Get(), *Map))
		{
			Map.Reset();
			return false;
		}
	}
	else if (!Map->Load(*Wad, MapName))
	{
		Map.Reset();
		return false;
	}
	// -buildnodes: play on the port's own node builder instead of the WAD's nodes (its test)
	static const bool bBuildNodes = FParse::Param(FCommandLine::Get(), TEXT("buildnodes"));
	if (bBuildNodes && !bEdited && Map->Rebuild())
	{
		UE_LOG(LogDoom, Log, TEXT("%s: nodes rebuilt by the node builder (%s): %d subsectors, %d nodes"), *Map->Name, *Map->NodesFormat, Map->Subsectors.Num(), Map->Nodes.Num());
	}
	// new level: the game state's counters and every player's share start from zero (a network
	// client only mirrors the server's record)
	if (!IsNetClient())
	{
		Flow().StartLevel(Map->Name);
		++Flow().LevelSerial;
		Flow().bDeathmatch = bDeathmatchParam && IsNetGame();
		if (ADoomGameState* GS = GetDoomGameState())
		{
			GS->ResetWorldNet();
			for (APlayerState* PS : GS->PlayerArray)
			{
				if (ADoomPlayerState* DPS = Cast<ADoomPlayerState>(PS)) DPS->StartLevel();
			}
		}
	}
	LoadedSerial = Flow().LevelSerial;
	// an imported Unreal level for this map brings its sector edits and thing markers
	if (const ADoomMapActor* Edits = bEdited ? nullptr : ADoomMapActor::Find(GetWorld()))
	{
		if (Edits->MapName.Equals(Flow().CurrentMap, ESearchCase::IgnoreCase))
		{
			Edits->ApplyEdits(*Map);
		}
	}
	ResetClock();
	bAutomap = false;
	bFullMap = false;

	TSet<int32> Movable;
	ComputeMovableSectors(*Map, Movable);

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Level = GetWorld()->SpawnActor<ADoomLevel>(ADoomLevel::StaticClass(), FTransform::Identity, SP);
	const FDoomMapInfo* Info = MapInfo(Flow().CurrentMap);

	PendingLoad = MakeShared<FDoomMapLoad>();
	FDoomMapLoad& L = *PendingLoad;
	L.bPistolStart = bPistolStart;
	L.bForSaveGame = bForSaveGame;
	L.StartTime = StartTime;
	L.Sky = (Info && !Info->Sky.IsEmpty()) ? FName(*Info->Sky) : FDoomGraphics::SkyTextureFor(Flow().CurrentMap);
	// the map's wall textures and flats decode on worker threads while the chunks are laid out
	L.Pictures = Gfx->CollectPrefetch(*Map);
	L.Task = UE::Tasks::Launch(UE_SOURCE_LOCATION, [G = Gfx.Get(), P = L.Pictures]() { G->DecodePrefetch(*P); });
	Level->TileSet = WadSetName;
	Level->BeginBuild(*Map, *Gfx, Movable, bModern, L.Sky);
	return true;
}

bool UDoomWorld::StepLoad(double Deadline)
{
	if (!PendingLoad.IsValid())
	{
		return true;
	}
	FDoomMapLoad& L = *PendingLoad;
	const bool bBlock = Deadline >= MAX_dbl;
	if (L.Stage == FDoomMapLoad::EStage::Pictures)
	{
		if (!L.Task.IsCompleted())
		{
			if (!bBlock) return false;
			L.Task.Wait();
		}
		// cached before the geometry starts, so its picture lookups on workers are read-only
		Gfx->CachePrefetch(*L.Pictures);
		L.PicturesTime = FPlatformTime::Seconds() - L.StartTime;
		L.Task = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Lv = Level.Get(), Out = &L.Sections]() { Lv->GenerateChunks(*Out); });
		L.Stage = FDoomMapLoad::EStage::Geometry;
	}
	if (L.Stage == FDoomMapLoad::EStage::Geometry)
	{
		// GPU textures on the game thread while the workers generate the geometry
		if (!Gfx->CreatePrefetchTextures(*L.Pictures, Deadline))
		{
			return false;
		}
		if (!L.Task.IsCompleted())
		{
			if (!bBlock) return false;
			L.Task.Wait();
		}
		L.GeometryTime = FPlatformTime::Seconds() - L.StartTime;
		L.Stage = FDoomMapLoad::EStage::Commit;
		if (FPlatformTime::Seconds() > Deadline)
		{
			return false;
		}
	}
	if (!Level->CommitChunks(L.Sections, L.NextChunk, Deadline))
	{
		return false;
	}
	Level->FinishBuild();
	const TSharedPtr<FDoomMapLoad> Done = MoveTemp(PendingLoad);
	FinishLoad(*Done);
	return true;
}

float UDoomWorld::LoadingProgress() const
{
	if (!PendingLoad.IsValid())
	{
		return 1.f;
	}
	const FDoomMapLoad& L = *PendingLoad;
	switch (L.Stage)
	{
	case FDoomMapLoad::EStage::Pictures:
		return 0.1f;
	case FDoomMapLoad::EStage::Geometry:
		return 0.2f + 0.4f * L.Pictures->NextToCreate / FMath::Max(1, L.Pictures->Items.Num());
	default:
		return 0.6f + 0.4f * L.NextChunk / FMath::Max(1, Level ? Level->NumChunks() : 1);
	}
}

void UDoomWorld::FinishLoad(const FDoomMapLoad& L)
{
	const bool bPistolStart = L.bPistolStart, bForSaveGame = L.bForSaveGame;
	const FDoomMapInfo* Info = MapInfo(Flow().CurrentMap);
	UE_LOG(LogDoom, Log, TEXT("%s: \"%s\", sky %s"), *Flow().CurrentMap, *LevelTitle(Flow().CurrentMap), *L.Sky.ToString());

	SetupEnvironment();
	BaselineSectors = Map->Sectors;
	BaselineSides = Map->Sides;
	if (!bForSaveGame && !IsNetClient())
	{
		// a save game brings its own lights, movers, monsters and things; network clients get
		// them from the server
		StartSectorSpecials();
		SpawnMapThings();
	}

	if (IsNetClient())
	{
		if (ADoomPlayer* P = GetPlayer())
		{
			P->InitForLevel(this, false);
		}
		bClientGeometryDirty = true;
	}
	else
	{
		for (ADoomPlayer* P : GetPlayers())
		{
			P->InitForLevel(this, bPistolStart);
		}
		Flow().Phase = EDoomGameState::Playing;
	}
	ShowMessage(LevelTitle(Flow().CurrentMap));
	PlayMusic((Info && !Info->Music.IsEmpty()) ? Info->Music : MusicForMap(Flow().CurrentMap));
	UE_LOG(LogDoom, Log, TEXT("Loaded %s in %.0f ms: %d pictures decoded by %.0f ms, geometry generated by %.0f ms"),
		*Flow().CurrentMap, (FPlatformTime::Seconds() - L.StartTime) * 1000, L.Pictures->Items.Num(), L.PicturesTime * 1000, L.GeometryTime * 1000);
}

void UDoomWorld::ComputeMovableSectors(const FDoomMap& InMap, TSet<int32>& Out)
{
	for (const FDoomLine& L : InMap.Lines)
	{
		if (L.Special == 0)
		{
			continue;
		}
		const DoomWorldDetail::FSpec* S = DoomWorldDetail::FindSpec(L.Special);
		if (!S)
		{
			continue;
		}
		const bool bLight = S->Act == DoomWorldDetail::EAct::LightMaxNeighbour || S->Act == DoomWorldDetail::EAct::LightMinNeighbour || S->Act == DoomWorldDetail::EAct::Light255
			|| S->Act == DoomWorldDetail::EAct::Light35 || S->Act == DoomWorldDetail::EAct::LightStrobe || S->Act == DoomWorldDetail::EAct::Teleport || S->Act == DoomWorldDetail::EAct::TeleportMonsterOnly
			|| S->Act == DoomWorldDetail::EAct::Exit || S->Act == DoomWorldDetail::EAct::SecretExit;
		if (bLight)
		{
			continue;
		}
		if (S->Trig == 'D')
		{
			if (L.BackSector != INDEX_NONE) Out.Add(L.BackSector);
			continue;
		}
		for (int32 Sec : InMap.SectorsWithTag(L.Tag))
		{
			Out.Add(Sec);
			if (S->Act == DoomWorldDetail::EAct::Stairs8 || S->Act == DoomWorldDetail::EAct::Stairs16)
			{
				// every sector the stairs can reach: follow same-flat neighbours
				TArray<int32> Stack = { Sec };
				while (Stack.Num())
				{
					const int32 Cur = Stack.Pop();
					for (int32 Li : InMap.Sectors[Cur].Lines)
					{
						const FDoomLine& L2 = InMap.Lines[Li];
						if (L2.FrontSector == Cur && L2.BackSector != INDEX_NONE && !Out.Contains(L2.BackSector)
							&& InMap.Sectors[L2.BackSector].FloorPic == InMap.Sectors[Sec].FloorPic)
						{
							Out.Add(L2.BackSector);
							Stack.Add(L2.BackSector);
						}
					}
				}
			}
			if (S->Act == DoomWorldDetail::EAct::Donut)
			{
				for (int32 Li : InMap.Sectors[Sec].Lines)
				{
					const int32 Ring = InMap.OtherSector(Li, Sec);
					if (Ring != INDEX_NONE) Out.Add(Ring);
				}
			}
		}
	}
	// sector specials that move: door close in 30 s, raise in 5 min
	for (int32 i = 0; i < InMap.Sectors.Num(); ++i)
	{
		if (InMap.Sectors[i].Special == 10 || InMap.Sectors[i].Special == 14)
		{
			Out.Add(i);
		}
	}
}

void UDoomWorld::SpawnMapThings()
{
	const int32 SkillBit = Skill <= 2 ? 1 : Skill == 3 ? 2 : 4;
	const bool bNet = IsNetGame();
	const bool bDM = IsDeathmatch();
	PlayerStarts.Reset();
	PlayerStarts.SetNum(4);
	DeathmatchStarts.Reset();
	for (const FDoomMapThing& T : Map->Things)
	{
		if (T.Type >= 1 && T.Type <= 4)
		{
			if (PlayerStarts[T.Type - 1].Type == 0) PlayerStarts[T.Type - 1] = T;
			continue;
		}
		if (T.Type == 11)
		{
			DeathmatchStarts.Add(T);
			continue;
		}
		if (T.Type == 87 || T.Type == 89)
		{
			// the Icon of Sin's spawn targets and spawn shooter (invisible)
			const int32 Sec = Map->SectorAt(T.P);
			const FVector Feet = Doom::ToUE(T.P, Sec >= 0 ? Map->Sectors[Sec].Floor : 0);
			(T.Type == 87 ? BrainTargets : BrainShooters).Add(Feet);
			continue;
		}
		if (T.Type == 14)
		{
			continue;
		}
		// flag 16: only in network games
		if (!(T.Flags & SkillBit) || ((T.Flags & 16) && !bNet))
		{
			continue;
		}
		const FDoomThingDef* Def = DoomInfo::FindThing(T.Type);
		if (!Def)
		{
			UE_LOG(LogDoom, Verbose, TEXT("Unknown thing type %d"), T.Type);
			continue;
		}
		if (bDM && Def->Pickup >= EPickup::BlueCard && Def->Pickup <= EPickup::RedSkull)
		{
			continue; // Doom's MF_NOTDMATCH: no keys in deathmatch
		}
		if ((Def->Flags & EThingFlag::Monster) && bNoMonsters)
		{
			continue; // -nomonsters
		}
		const int32 Sec = Map->SectorAt(T.P);
		const float Floor = Sec >= 0 ? Map->Sectors[Sec].Floor : 0;
		if (Def->Flags & EThingFlag::Monster)
		{
			if (ADoomMonster* M = SpawnMonster(T.Type, Doom::ToUE(T.P, Floor), T.Angle))
			{
				M->bAmbush = (T.Flags & 8) != 0;
				M->SpawnFeet = Doom::ToUE(T.P, Floor);
				M->SpawnAngle = T.Angle;
				M->bCanRespawn = (M->Def->Flags & EThingFlag::CountKill) != 0;
			}
		}
		else
		{
			SpawnThing(T.Type, Doom::ToUE(T.P, Floor), T.Angle);
		}
	}
	for (ADoomPlayer* P : GetPlayers())
	{
		PlacePlayer(P);
	}
	UE_LOG(LogDoom, Log, TEXT("Spawned %d things, %d monsters"), Things.Num(), Monsters.Num());
}

// ------------------------------------------------------------------------------------------------
// Spawning
// ------------------------------------------------------------------------------------------------

ADoomThing* UDoomWorld::SpawnThing(int32 DoomEdNum, const FVector& UEFeet, float DoomAngle, bool bDropped)
{
	const FDoomThingDef* Def = DoomInfo::FindThing(DoomEdNum);
	if (!Def)
	{
		return nullptr;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SP.bDeferConstruction = false;
	ADoomThing* T = GetWorld()->SpawnActor<ADoomThing>(ADoomThing::StaticClass(), FTransform(UEFeet + FVector(0, 0, Def->Height * 0.5f * Doom::Scale)), SP);
	if (T)
	{
		T->InitThing(this, Def, DoomAngle, bDropped);
		RegisterThing(T);
		if ((Def->Flags & EThingFlag::CountItem) && !bDropped)
		{
			++Flow().TotalItems;
		}
	}
	return T;
}

ADoomThing* UDoomWorld::SpawnEffect(const FString& SpriteName, const FString& Frames, int32 Tics, const FVector& UEPos, bool bFullBright, bool bLoop)
{
	if (IsNetServer())
	{
		if (ADoomGameState* GS = GetDoomGameState())
		{
			GS->MulticastEffect(SpriteName, Frames, (uint8)FMath::Clamp(Tics, 1, 255), UEPos, bFullBright, bLoop);
		}
	}
	return SpawnEffectLocal(SpriteName, Frames, Tics, UEPos, bFullBright, bLoop);
}

ADoomThing* UDoomWorld::SpawnEffectLocal(const FString& SpriteName, const FString& Frames, int32 Tics, const FVector& UEPos, bool bFullBright, bool bLoop)
{
	if (!Gfx.IsValid())
	{
		return nullptr;
	}
	ADoomThing* T = nullptr;
	while (!T && EffectPool.Num())
	{
		T = EffectPool.Pop(EAllowShrinking::No).Get();
	}
	if (T)
	{
		++EffectsReused;
		T->bPooled = false;
		T->SetActorLocation(UEPos, false, nullptr, ETeleportType::ResetPhysics);
		T->SetActorHiddenInGame(false);
	}
	else
	{
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		T = GetWorld()->SpawnActor<ADoomThing>(ADoomThing::StaticClass(), FTransform(UEPos), SP);
		EffectsSpawned += T ? 1 : 0;
	}
	if (T)
	{
		T->InitEffect(this, SpriteName, Frames, Tics, bFullBright, bLoop);
		RegisterThing(T);
	}
	// hit effects get particles too, sprayed back towards the viewer (who usually fired the shot);
	// missile trail puffs ("BCD") stay plain
	if (bModern && (SpriteName == TEXT("BLUD") || (SpriteName == TEXT("PUFF") && Frames == TEXT("ABCD")) || SpriteName == TEXT("BFE2")))
	{
		const ADoomPlayer* P = GetPlayer();
		const FVector Back = P ? (P->GetActorLocation() - UEPos).GetSafeNormal() : FVector::UpVector;
		const EDoomParticles Kind = SpriteName == TEXT("BLUD") ? EDoomParticles::Blood : SpriteName == TEXT("BFE2") ? EDoomParticles::BFG : EDoomParticles::Sparks;
		SpawnParticlesLocal(Kind, UEPos, (Back + FVector(0, 0, 0.5f)).GetSafeNormal());
	}
	return T;
}

void UDoomWorld::SpawnParticles(EDoomParticles Kind, const FVector& UEPos, const FVector& Up)
{
	if (IsNetServer())
	{
		if (ADoomGameState* GS = GetDoomGameState())
		{
			GS->MulticastParticles((uint8)Kind, UEPos, Up);
		}
	}
	SpawnParticlesLocal(Kind, UEPos, Up);
}

void UDoomWorld::SpawnParticlesLocal(EDoomParticles Kind, const FVector& UEPos, const FVector& Up)
{
	if (!bModern || bNoParticles)
	{
		return;
	}
	if (UNiagaraSystem* Sys = DoomAssets::Particles(Kind))
	{
		// the bursts spray along the emitter's +Z
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), Sys, UEPos, FRotationMatrix::MakeFromZ(Up).Rotator(), FVector::OneVector,
			true, true, ENCPoolMethod::AutoRelease);
		++ParticleBursts;
	}
}

ADoomMonster* UDoomWorld::SpawnMonster(int32 DoomEdNum, const FVector& UEFeet, float DoomAngle)
{
	const FDoomThingDef* Def = DoomInfo::FindThing(DoomEdNum);
	if (!Def)
	{
		return nullptr;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	FVector Pos = UEFeet + FVector(0, 0, Def->Height * 0.5f * Doom::Scale + 1);
	if (Def->Flags & EThingFlag::Hanging)
	{
		const float Ceil = CeilingAt(UEFeet);
		Pos.Z = (Ceil - Def->Height * 0.5f) * Doom::Scale;
	}
	ADoomMonster* M = GetWorld()->SpawnActor<ADoomMonster>(ADoomMonster::StaticClass(), FTransform(FRotator(0, Doom::AngleToYaw(DoomAngle), 0), Pos), SP);
	if (M)
	{
		M->InitMonster(this, Def, DoomAngle, false);
		RegisterMonster(M);
		if (Def->Flags & EThingFlag::CountKill)
		{
			++Flow().TotalKills;
		}
	}
	return M;
}

ADoomProjectile* UDoomWorld::SpawnProjectile(const FString& Type, AActor* Source, const FVector& From, const FVector& Direction, AActor* Tracer, bool bSilent)
{
	const FDoomProjectileDef* Def = DoomInfo::FindProjectile(Type);
	if (!Def)
	{
		return nullptr;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SP.Owner = Source;
	SP.Instigator = Cast<APawn>(Source);
	ADoomProjectile* P = GetWorld()->SpawnActor<ADoomProjectile>(ADoomProjectile::StaticClass(), FTransform(Direction.Rotation(), From), SP);
	if (P)
	{
		P->InitProjectile(this, Def, Source, Direction.GetSafeNormal(), Tracer);
		RegisterProjectile(P);
		if (!bSilent)
		{
			PlaySound(Def->FireSound, &From);
		}
	}
	return P;
}

void UDoomWorld::RegisterThing(ADoomThing* T) { Things.Add(T); }

void UDoomWorld::ReleaseEffect(ADoomThing* Effect)
{
	if (!Effect || Effect->bPooled)
	{
		return;
	}
	if (EffectPool.Num() >= 64 || Effect->IsActorBeingDestroyed() || Doom::PerfBaseline())
	{
		Effect->Destroy();
		return;
	}
	Effect->bPooled = true;
	Effect->SetActorHiddenInGame(true);
	Effect->SetActorTickEnabled(false);
	if (Effect->Light) Effect->Light->SetVisibility(false);
	EffectPool.Add(Effect);
}
void UDoomWorld::RegisterMonster(ADoomMonster* M) { Monsters.Add(M); }
void UDoomWorld::RegisterProjectile(ADoomProjectile* P) { Projectiles.Add(P); }

AActor* UDoomWorld::DamageSource(AActor* DamageCauser)
{
	if (const ADoomProjectile* Missile = Cast<ADoomProjectile>(DamageCauser))
	{
		return Missile->Source.IsValid() ? Missile->Source.Get() : DamageCauser;
	}
	if (const ADoomThing* Barrel = Cast<ADoomThing>(DamageCauser))
	{
		return Barrel->LastAttacker.IsValid() ? Barrel->LastAttacker.Get() : DamageCauser;
	}
	if (const AController* C = Cast<AController>(DamageCauser))
	{
		return C->GetPawn();
	}
	return DamageCauser;
}

void UDoomWorld::RadiusAttack(AActor* Source, AActor* Instigator, const FVector& Where, float Damage)
{
	// Doom's P_RadiusAttack: Damage - distance, where distance is the larger of |dx| and |dy| minus the
	// victim's radius (heights ignored, as in vanilla); needs a clear line from the blast. Unreal's radial
	// damage passes the full base damage to TakeDamage overrides, so the falloff is done here.
	FCollisionObjectQueryParams Walls;
	Walls.AddObjectTypesToQuery(ECC_WorldStatic);
	const FVector2f Spot = Doom::ToDoom2D(Where);
	AController* InstigatorController = Instigator ? Instigator->GetInstigatorController() : nullptr;
	auto Hit = [&](AActor* Victim, float Radius)
	{
		const FVector2f D = Doom::ToDoom2D(Victim->GetActorLocation()) - Spot;
		const float Dist = FMath::Max(0.f, FMath::Max(FMath::Abs(D.X), FMath::Abs(D.Y)) - Radius);
		if (Dist >= Damage)
		{
			return;
		}
		FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomBlast), false, Source);
		Q.AddIgnoredActor(Victim);
		FHitResult Block;
		if (GetWorld()->LineTraceSingleByObjectType(Block, Where, Victim->GetActorLocation(), Walls, Q))
		{
			return;
		}
		FRadialDamageEvent Event;
		Event.Origin = Where;
		Event.Params = FRadialDamageParams(Damage, Damage * Doom::Scale);
		Victim->TakeDamage(FMath::FloorToFloat(Damage - Dist), Event, InstigatorController, Source);
	};
	TArray<TPair<AActor*, float>> Victims;
	for (ADoomPlayer* P : GetPlayers())
	{
		if (!P->IsDead()) Victims.Add({ P, 16.f });
	}
	for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
	{
		if (M.IsValid() && !M->IsDead()) Victims.Add({ M.Get(), M->Def->Radius });
	}
	for (const TWeakObjectPtr<ADoomThing>& T : Things)
	{
		if (T.IsValid() && T.Get() != Source && T->Def && (T->Def->Flags & EThingFlag::Shootable) && !T->bDead) Victims.Add({ T.Get(), T->Def->Radius });
	}
	for (const TPair<AActor*, float>& V : Victims)
	{
		if (IsValid(V.Key)) Hit(V.Key, V.Value);
	}
}

bool UDoomWorld::CheckSight(const AActor* From, const AActor* To) const
{
	++SightChecks;
	if (!From || !To)
	{
		return false;
	}
	// P_CheckSight: REJECT rules most pairs out without a trace
	if (Map.IsValid() && Map->Reject.Num() && !Doom::PerfBaseline() && Map->RejectsSight(SectorAt(From->GetActorLocation()), SectorAt(To->GetActorLocation())))
	{
		++RejectedSights;
		return false;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomSight), false, From);
	Q.AddIgnoredActor(To);
	const FVector A = From->GetActorLocation() + FVector(0, 0, Doom::HalfHeight(From) * 0.5f);
	const FVector B = To->GetActorLocation() + FVector(0, 0, Doom::HalfHeight(To) * 0.5f);
	FHitResult Hit;
	// monsters do not block sight; only the level does
	FCollisionObjectQueryParams Obj;
	Obj.AddObjectTypesToQuery(ECC_WorldStatic);
	return !GetWorld()->LineTraceSingleByObjectType(Hit, A, B, Obj, Q);
}

// ------------------------------------------------------------------------------------------------
// Queries and sounds
// ------------------------------------------------------------------------------------------------

int32 UDoomWorld::SectorAt(const FVector& UEPos) const
{
	return Map.IsValid() ? Map->SectorAt(Doom::ToDoom2D(UEPos)) : INDEX_NONE;
}

float UDoomWorld::FloorAt(const FVector& UEPos) const
{
	const int32 S = SectorAt(UEPos);
	return S >= 0 ? Map->Sectors[S].Floor : 0.f;
}

float UDoomWorld::CeilingAt(const FVector& UEPos) const
{
	const int32 S = SectorAt(UEPos);
	return S >= 0 ? Map->Sectors[S].Ceiling : 128.f;
}

// ------------------------------------------------------------------------------------------------
// Tic
// ------------------------------------------------------------------------------------------------

void UDoomWorld::QueueRespawn(int32 DoomEdNum, const FVector& UEFeet, float DoomAngle)
{
	ItemRespawns.Add({ DoomEdNum, UEFeet, DoomAngle, Flow().LevelTic + 30 * Doom::TicRate });
}

void UDoomWorld::RunTic()
{
	++Flow().LevelTic;
	if (MessageTics > 0) --MessageTics;
	// -altdeath: items back after 30 s, with the teleport fog and its sound (P_RespawnSpecials)
	for (int32 i = ItemRespawns.Num() - 1; i >= 0; --i)
	{
		const FItemRespawn R = ItemRespawns[i];
		if (Flow().LevelTic < R.Tic) continue;
		ItemRespawns.RemoveAtSwap(i);
		if (SpawnThing(R.Type, R.Feet, R.Angle))
		{
			SpawnEffect(TEXT("IFOG"), TEXT("ABCDE"), 6, R.Feet + FVector(0, 0, 16.f * Doom::Scale));
			PlaySound(TEXT("ITMBK"), &R.Feet);
		}
	}

	for (ADoomPlayer* P : GetPlayers())
	{
		P->DoomTic();
	}
	for (int32 i = Monsters.Num() - 1; i >= 0; --i)
	{
		if (Monsters[i].IsValid()) Monsters[i]->DoomTic();
		else Monsters.RemoveAtSwap(i);
	}
	for (int32 i = Things.Num() - 1; i >= 0; --i)
	{
		if (Things[i].IsValid() && !Things[i]->bPooled) Things[i]->DoomTic();
		if (!Things.IsValidIndex(i)) continue;
		if (!Things[i].IsValid() || Things[i]->bPooled) Things.RemoveAtSwap(i);
	}
	for (int32 i = Projectiles.Num() - 1; i >= 0; --i)
	{
		if (Projectiles[i].IsValid()) Projectiles[i]->DoomTic();
		else Projectiles.RemoveAtSwap(i);
	}
	if (Flow().Phase != EDoomGameState::Playing)
	{
		return;
	}
	TicMovers();
	TicLights();
	TicSectorDamage();
	TicBrain();
	TicRespawn();
	if (Flow().Phase != EDoomGameState::Playing)
	{
		return;
	}

	// switch buttons popping back
	for (int32 i = Buttons.Num() - 1; i >= 0; --i)
	{
		if (--Buttons[i].Timer <= 0)
		{
			FSwitchButton& B = Buttons[i];
			FDoomSide& S = Map->Sides[Map->Lines[B.Line].Side[B.Side]];
			(B.Part == 0 ? S.Upper : B.Part == 1 ? S.Middle : S.Lower) = B.Original;
			Level->LineChanged(B.Line);
			const FVector2f C = (Map->V(Map->Lines[B.Line].V1) + Map->V(Map->Lines[B.Line].V2)) * 0.5f;
			const FVector Pos = Doom::ToUE(C, FloorAt(Doom::ToUE(C, 0)) + 32);
			PlaySound(TEXT("SWTCHN"), &Pos);
			Buttons.RemoveAtSwap(i);
		}
	}

	Level->AnimateTextures(Flow().LevelTic);
	Level->Flush();
	if (bLightsDirty)
	{
		Level->UpdateSectorLights();
		bLightsDirty = false;
	}
}

void UDoomWorld::SetSmartAI(bool bOn)
{
	bSmartAI = bOn;
	for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
	{
		if (M.IsValid()) M->SetSmartAI(bOn);
	}
	UE_LOG(LogDoom, Log, TEXT("Monster AI: %s"), bOn ? TEXT("SMART (Behavior Tree, EQS, perception)") : TEXT("CLASSIC"));
}

const FDoomNav& UDoomWorld::GetNav() const
{
	if (!bNavBuilt && Map.IsValid())
	{
		const double T0 = FPlatformTime::Seconds();
		Nav.Build(*Map);
		bNavBuilt = true;
		UE_LOG(LogDoom, Log, TEXT("%s: monster paths over %d subsectors, %d portals, in %.1f ms"), *Map->Name, Map->Subsectors.Num(), Nav.NumPortals(), (FPlatformTime::Seconds() - T0) * 1000.0);
	}
	return Nav;
}

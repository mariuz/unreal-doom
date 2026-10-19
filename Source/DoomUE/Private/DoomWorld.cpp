#include "DoomWorld.h"
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
// Linedef special table (vanilla Doom numbers)
// ------------------------------------------------------------------------------------------------
namespace
{
	enum class EAct : uint8
	{
		DoorRaise, DoorOpen, DoorClose, DoorClose30, DoorBlazeRaise, DoorBlazeOpen, DoorBlazeClose,
		Lift, LiftBlaze, PlatPerpetual, PlatStop, PlatRaise24Change, PlatRaise32Change, PlatRaiseNextChange,
		FloorRaiseLowestCeil, FloorRaiseCrush, FloorRaiseNext, FloorRaiseNextTurbo, FloorLowerHighest, FloorLowerHighestTurbo,
		FloorLowerLowest, FloorLowerLowestChange, FloorRaise24, FloorRaise24Change, FloorRaise512, FloorRaiseShortest,
		CeilRaiseHighest, CeilLowerFloor, CeilLower8, Crusher, CrusherFast, CrusherSilent, CrusherStop,
		Stairs8, Stairs16, LightMaxNeighbour, LightMinNeighbour, Light255, Light35, LightStrobe,
		Teleport, TeleportMonsterOnly, Exit, SecretExit, Donut
	};

	struct FSpec
	{
		int16 Num;
		TCHAR Trig;      // D manual door, W walk, S switch, G gun
		bool bRepeat;
		EAct Act;
		int8 Lock = 0;   // 1 blue 2 yellow 3 red
		bool bMonster = false;
	};

	const FSpec GSpecs[] =
	{
		// manual doors
		{ 1, 'D', true, EAct::DoorRaise, 0, true }, { 26, 'D', true, EAct::DoorRaise, 1 }, { 27, 'D', true, EAct::DoorRaise, 2 }, { 28, 'D', true, EAct::DoorRaise, 3 },
		{ 31, 'D', false, EAct::DoorOpen }, { 32, 'D', false, EAct::DoorOpen, 1 }, { 33, 'D', false, EAct::DoorOpen, 3 }, { 34, 'D', false, EAct::DoorOpen, 2 },
		{ 117, 'D', true, EAct::DoorBlazeRaise }, { 118, 'D', false, EAct::DoorBlazeOpen },
		// tagged doors
		{ 2, 'W', false, EAct::DoorOpen }, { 3, 'W', false, EAct::DoorClose }, { 4, 'W', false, EAct::DoorRaise, 0, true }, { 16, 'W', false, EAct::DoorClose30 },
		{ 29, 'S', false, EAct::DoorRaise }, { 42, 'S', true, EAct::DoorClose }, { 46, 'G', true, EAct::DoorOpen }, { 50, 'S', false, EAct::DoorClose },
		{ 61, 'S', true, EAct::DoorOpen }, { 63, 'S', true, EAct::DoorRaise }, { 75, 'W', true, EAct::DoorClose }, { 76, 'W', true, EAct::DoorClose30 },
		{ 86, 'W', true, EAct::DoorOpen }, { 90, 'W', true, EAct::DoorRaise }, { 103, 'S', false, EAct::DoorOpen },
		{ 105, 'W', true, EAct::DoorBlazeRaise }, { 106, 'W', true, EAct::DoorBlazeOpen }, { 107, 'W', true, EAct::DoorBlazeClose },
		{ 108, 'W', false, EAct::DoorBlazeRaise }, { 109, 'W', false, EAct::DoorBlazeOpen }, { 110, 'W', false, EAct::DoorBlazeClose },
		{ 111, 'S', false, EAct::DoorBlazeRaise }, { 112, 'S', false, EAct::DoorBlazeOpen }, { 113, 'S', false, EAct::DoorBlazeClose },
		{ 114, 'S', true, EAct::DoorBlazeRaise }, { 115, 'S', true, EAct::DoorBlazeOpen }, { 116, 'S', true, EAct::DoorBlazeClose },
		{ 99, 'S', true, EAct::DoorBlazeOpen, 1 }, { 133, 'S', false, EAct::DoorBlazeOpen, 1 }, { 134, 'S', true, EAct::DoorBlazeOpen, 3 },
		{ 135, 'S', false, EAct::DoorBlazeOpen, 3 }, { 136, 'S', true, EAct::DoorBlazeOpen, 2 }, { 137, 'S', false, EAct::DoorBlazeOpen, 2 },
		// lifts and plats
		{ 10, 'W', false, EAct::Lift, 0, true }, { 21, 'S', false, EAct::Lift }, { 62, 'S', true, EAct::Lift }, { 88, 'W', true, EAct::Lift, 0, true },
		{ 120, 'W', true, EAct::LiftBlaze }, { 121, 'W', false, EAct::LiftBlaze }, { 122, 'S', false, EAct::LiftBlaze }, { 123, 'S', true, EAct::LiftBlaze },
		{ 53, 'W', false, EAct::PlatPerpetual }, { 87, 'W', true, EAct::PlatPerpetual }, { 54, 'W', false, EAct::PlatStop }, { 89, 'W', true, EAct::PlatStop },
		{ 14, 'S', false, EAct::PlatRaise32Change }, { 15, 'S', false, EAct::PlatRaise24Change }, { 66, 'S', true, EAct::PlatRaise24Change },
		{ 67, 'S', true, EAct::PlatRaise32Change }, { 20, 'S', false, EAct::PlatRaiseNextChange }, { 22, 'W', false, EAct::PlatRaiseNextChange },
		{ 47, 'G', false, EAct::PlatRaiseNextChange }, { 68, 'S', true, EAct::PlatRaiseNextChange }, { 95, 'W', true, EAct::PlatRaiseNextChange },
		// floors
		{ 5, 'W', false, EAct::FloorRaiseLowestCeil }, { 91, 'W', true, EAct::FloorRaiseLowestCeil }, { 101, 'S', false, EAct::FloorRaiseLowestCeil },
		{ 64, 'S', true, EAct::FloorRaiseLowestCeil }, { 24, 'G', false, EAct::FloorRaiseLowestCeil },
		{ 55, 'S', false, EAct::FloorRaiseCrush }, { 56, 'W', false, EAct::FloorRaiseCrush }, { 65, 'S', true, EAct::FloorRaiseCrush }, { 94, 'W', true, EAct::FloorRaiseCrush },
		{ 18, 'S', false, EAct::FloorRaiseNext }, { 69, 'S', true, EAct::FloorRaiseNext }, { 119, 'W', false, EAct::FloorRaiseNext }, { 128, 'W', true, EAct::FloorRaiseNext },
		{ 129, 'W', true, EAct::FloorRaiseNextTurbo }, { 130, 'W', false, EAct::FloorRaiseNextTurbo }, { 131, 'S', false, EAct::FloorRaiseNextTurbo }, { 132, 'S', true, EAct::FloorRaiseNextTurbo },
		{ 19, 'W', false, EAct::FloorLowerHighest }, { 45, 'S', true, EAct::FloorLowerHighest }, { 83, 'W', true, EAct::FloorLowerHighest }, { 102, 'S', false, EAct::FloorLowerHighest },
		{ 36, 'W', false, EAct::FloorLowerHighestTurbo }, { 70, 'S', true, EAct::FloorLowerHighestTurbo }, { 71, 'S', false, EAct::FloorLowerHighestTurbo }, { 98, 'W', true, EAct::FloorLowerHighestTurbo },
		{ 38, 'W', false, EAct::FloorLowerLowest }, { 23, 'S', false, EAct::FloorLowerLowest }, { 60, 'S', true, EAct::FloorLowerLowest }, { 82, 'W', true, EAct::FloorLowerLowest },
		{ 37, 'W', false, EAct::FloorLowerLowestChange }, { 84, 'W', true, EAct::FloorLowerLowestChange },
		{ 58, 'W', false, EAct::FloorRaise24 }, { 92, 'W', true, EAct::FloorRaise24 }, { 59, 'W', false, EAct::FloorRaise24Change }, { 93, 'W', true, EAct::FloorRaise24Change },
		{ 140, 'S', false, EAct::FloorRaise512 }, { 30, 'W', false, EAct::FloorRaiseShortest }, { 96, 'W', true, EAct::FloorRaiseShortest },
		{ 9, 'S', false, EAct::Donut },
		// ceilings and crushers
		{ 40, 'W', false, EAct::CeilRaiseHighest }, { 41, 'S', false, EAct::CeilLowerFloor }, { 43, 'S', true, EAct::CeilLowerFloor },
		{ 44, 'W', false, EAct::CeilLower8 }, { 72, 'W', true, EAct::CeilLower8 },
		{ 25, 'W', false, EAct::Crusher }, { 73, 'W', true, EAct::Crusher }, { 49, 'S', false, EAct::Crusher },
		{ 6, 'W', false, EAct::CrusherFast }, { 77, 'W', true, EAct::CrusherFast }, { 141, 'W', false, EAct::CrusherSilent },
		{ 57, 'W', false, EAct::CrusherStop }, { 74, 'W', true, EAct::CrusherStop },
		// stairs
		{ 8, 'W', false, EAct::Stairs8 }, { 7, 'S', false, EAct::Stairs8 }, { 100, 'W', false, EAct::Stairs16 }, { 127, 'S', false, EAct::Stairs16 },
		// lights
		{ 12, 'W', false, EAct::LightMaxNeighbour }, { 80, 'W', true, EAct::LightMaxNeighbour }, { 104, 'W', false, EAct::LightMinNeighbour },
		{ 13, 'W', false, EAct::Light255 }, { 81, 'W', true, EAct::Light255 }, { 138, 'S', true, EAct::Light255 },
		{ 35, 'W', false, EAct::Light35 }, { 79, 'W', true, EAct::Light35 }, { 139, 'S', true, EAct::Light35 }, { 17, 'W', false, EAct::LightStrobe },
		// teleports and exits
		{ 39, 'W', false, EAct::Teleport, 0, true }, { 97, 'W', true, EAct::Teleport, 0, true },
		{ 125, 'W', false, EAct::TeleportMonsterOnly, 0, true }, { 126, 'W', true, EAct::TeleportMonsterOnly, 0, true },
		{ 11, 'S', false, EAct::Exit }, { 52, 'W', false, EAct::Exit }, { 51, 'S', false, EAct::SecretExit }, { 124, 'W', false, EAct::SecretExit },
	};

	const FSpec* FindSpec(int32 Num)
	{
		static TMap<int32, const FSpec*> Index;
		if (Index.Num() == 0)
		{
			for (const FSpec& S : GSpecs) Index.Add(S.Num, &S);
		}
		const FSpec* const* F = Index.Find(Num);
		return F ? *F : nullptr;
	}

	constexpr float DoorSpeed = 2.f;
	constexpr int32 DoorWait = 150;
	constexpr int32 PlatWait = 105;

	enum EDoorKind { DK_Raise, DK_Open, DK_Close, DK_Close30, DK_RaiseIn5Min, DK_CloseIn30 };
	enum EPlatKind { PK_Lift, PK_Perpetual, PK_RaiseChange, PK_RaiseNextChange };
	enum EFloorKind { FK_RaiseLowestCeil, FK_RaiseCrush, FK_RaiseNext, FK_LowerHighest, FK_LowerHighestPlus8, FK_LowerLowest, FK_LowerLowestChange, FK_Raise24, FK_Raise24Change, FK_Raise512, FK_RaiseShortest };
	enum ECeilKind { CK_RaiseHighest, CK_LowerFloor, CK_Lower8, CK_Crusher, CK_CrusherSilent };
}

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
	bNoParticles = FParse::Param(FCommandLine::Get(), TEXT("noparticles"));
	bDeathmatchParam = FParse::Param(FCommandLine::Get(), TEXT("deathmatch"));
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
	for (TWeakObjectPtr<ADoomMonster>& M : Monsters) if (M.IsValid()) M->Destroy();
	for (TWeakObjectPtr<ADoomProjectile>& P : Projectiles) if (P.IsValid()) P->Destroy();
	for (AActor* A : EnvironmentActors) if (A) A->Destroy();
	Things.Reset();
	Monsters.Reset();
	Projectiles.Reset();
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
	Map = MakeUnique<FDoomMap>();
	uint32 GenSeed = 0;
	if (DoomMapGen::ParseName(MapName, GenSeed))
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
	if (bBuildNodes && Map->Rebuild())
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
	if (const ADoomMapActor* Edits = ADoomMapActor::Find(GetWorld()))
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
		const FSpec* S = FindSpec(L.Special);
		if (!S)
		{
			continue;
		}
		const bool bLight = S->Act == EAct::LightMaxNeighbour || S->Act == EAct::LightMinNeighbour || S->Act == EAct::Light255
			|| S->Act == EAct::Light35 || S->Act == EAct::LightStrobe || S->Act == EAct::Teleport || S->Act == EAct::TeleportMonsterOnly
			|| S->Act == EAct::Exit || S->Act == EAct::SecretExit;
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
			if (S->Act == EAct::Stairs8 || S->Act == EAct::Stairs16)
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
			if (S->Act == EAct::Donut)
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

void UDoomWorld::SetupEnvironment()
{
	UWorld* W = GetWorld();
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	PostProcess = W->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FTransform::Identity, SP);
	PostProcess->bUnbound = true;
	FPostProcessSettings& PP = PostProcess->Settings;
	PP.bOverride_AutoExposureMethod = true;
	PP.AutoExposureMethod = AEM_Manual;
	PP.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	PP.AutoExposureApplyPhysicalCameraExposure = false;
	PP.bOverride_MotionBlurAmount = true;
	PP.MotionBlurAmount = 0.f;
	PP.bOverride_VignetteIntensity = true;
	PP.VignetteIntensity = 0.f;
	PP.bOverride_LensFlareIntensity = true;
	PP.LensFlareIntensity = 0.f;

	if (!bModern)
	{
		// Classic: the material already outputs Doom's palette colours; show them 1:1.
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = 0.263f; // log2(1.2): undo the lens attenuation of manual exposure
		PP.bOverride_ToneCurveAmount = true;
		PP.ToneCurveAmount = 0.f;
		PP.bOverride_ExpandGamut = true;
		PP.ExpandGamut = 0.f;
		PP.bOverride_BlueCorrection = true;
		PP.BlueCorrection = 0.f;
		PP.bOverride_BloomIntensity = true;
		PP.BloomIntensity = 0.f;
		PP.bOverride_AmbientOcclusionIntensity = true;
		PP.AmbientOcclusionIntensity = 0.f;
		PP.bOverride_DynamicGlobalIlluminationMethod = true;
		PP.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
		PP.bOverride_ReflectionMethod = true;
		PP.ReflectionMethod = EReflectionMethod::None;
		return;
	}

	// Modern: Lumen GI + reflections lit by emissive sector light, dynamic lights, fog and bloom.
	PP.bOverride_AutoExposureBias = true;
	PP.AutoExposureBias = 1.0f;
	PP.bOverride_DynamicGlobalIlluminationMethod = true;
	PP.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
	PP.bOverride_ReflectionMethod = true;
	PP.ReflectionMethod = EReflectionMethod::Lumen;
	PP.bOverride_LumenSceneLightingQuality = true;
	PP.LumenSceneLightingQuality = 1.f;
	PP.bOverride_LumenFinalGatherQuality = true;
	PP.LumenFinalGatherQuality = 1.f;
	PP.bOverride_BloomIntensity = true;
	PP.BloomIntensity = 0.5f;
	PP.bOverride_AmbientOcclusionIntensity = true;
	PP.AmbientOcclusionIntensity = 0.5f;

	AExponentialHeightFog* Fog = W->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(), FTransform(FVector(0, 0, -2000)), SP);
	UExponentialHeightFogComponent* FC = Fog->GetComponent();
	FC->SetFogDensity(0.002f);
	FC->SetFogHeightFalloff(0.02f);
	FC->SetVolumetricFog(true);
	FC->SetFogInscatteringColor(FLinearColor(0.05f, 0.05f, 0.06f));
	EnvironmentActors.Add(Fog);

	ADirectionalLight* Sun = W->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FTransform(FRotator(-55, 35, 0), FVector::ZeroVector), SP);
	Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sun->GetLightComponent()->SetIntensity(2.5f);
	Sun->GetLightComponent()->SetLightColor(FLinearColor(1.f, 0.85f, 0.7f));
	Sun->GetLightComponent()->SetVolumetricScatteringIntensity(1.f);
	EnvironmentActors.Add(Sun);

	ASkyLight* Sky = W->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FTransform::Identity, SP);
	USkyLightComponent* SC = Sky->GetLightComponent();
	SC->SetMobility(EComponentMobility::Movable);
	SC->bRealTimeCapture = true;
	SC->SetIntensity(0.4f);
	SC->RecaptureSky();
	EnvironmentActors.Add(Sky);
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
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ADoomThing* T = GetWorld()->SpawnActor<ADoomThing>(ADoomThing::StaticClass(), FTransform(UEPos), SP);
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
	if (!From || !To)
	{
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

void UDoomWorld::PlaySound(const FString& Name, const FVector* Location, float Volume)
{
	PlaySoundLocal(Name, Location, Volume);
	if (IsNetServer())
	{
		if (ADoomGameState* GS = GetDoomGameState())
		{
			GS->MulticastSound(Name, Location ? *Location : FVector::ZeroVector, Location != nullptr, Volume, nullptr);
		}
	}
}

void UDoomWorld::PlaySoundLocal(const FString& Name, const FVector* Location, float Volume)
{
	if (Sound.IsValid())
	{
		Sound->Play(GetWorld(), Name, Location, Volume);
	}
}

bool UDoomWorld::IsPaletteExact() const
{
	return !bModern && GetDefault<UDoomSettings>()->bPaletteExact && !FParse::Param(FCommandLine::Get(), TEXT("smooth"));
}

int32 UDoomWorld::ScreenPalette() const
{
	const ADoomPlayer* P = GetPlayer();
	if (!P || !Wad.IsValid() || Flow().Phase != EDoomGameState::Playing)
	{
		return 0;
	}
	static int32 Forced = -1;
	static const bool bForced = FParse::Value(FCommandLine::Get(), TEXT("-forcepalette="), Forced);
	if (bForced)
	{
		return FMath::Clamp(Forced, 0, Wad->NumPalettes() - 1); // screenshots of a given palette
	}
	int32 Palette = 0;
	int32 Hurt = P->DamageCount;
	if (P->bBerserk)
	{
		Hurt = FMath::Max(Hurt, 12 - (P->BerserkTics >> 6)); // berserk's fading red
	}
	if (Hurt > 0)
	{
		Palette = 1 + FMath::Min((Hurt + 7) >> 3, 7);
	}
	else if (P->BonusCount > 0)
	{
		Palette = 9 + FMath::Min((P->BonusCount + 7) >> 3, 3);
	}
	else if (P->RadSuit > 4 * 32 || (P->RadSuit & 8))
	{
		Palette = 13;
	}
	return FMath::Clamp(Palette, 0, Wad->NumPalettes() - 1);
}

void UDoomWorld::ApplyUserSettings()
{
	const UDoomSettings* S = GetDefault<UDoomSettings>();
	if (Sound.IsValid())
	{
		Sound->MasterVolume = S->SfxVolume;
	}
	if (MusicWave && MusicWave->GetPlayer().IsValid())
	{
		MusicWave->GetPlayer()->Volume = S->MusicVolume;
	}
	if (S->MusicVolume <= 0.f)
	{
		StopMusic();
	}
	else if (!MusicComponent && Map.IsValid())
	{
		const FDoomMapInfo* Info = MapInfo(Flow().CurrentMap);
		PlayMusic((Info && !Info->Music.IsEmpty()) ? Info->Music : MusicForMap(Flow().CurrentMap));
	}
	if (ADoomPlayer* P = GetPlayer())
	{
		P->Camera->SetFieldOfView(S->FieldOfView);
	}
}

void UDoomWorld::StopMusic()
{
	if (MusicComponent)
	{
		MusicComponent->Stop();
		MusicComponent->DestroyComponent();
		MusicComponent = nullptr;
	}
	MusicWave = nullptr;
	CurrentMusic.Reset();
}

void UDoomWorld::PlayTitleMusic()
{
	if (Wad.IsValid())
	{
		PlayMusic(Wad->IsDoom2() ? TEXT("D_DM2TTL") : TEXT("D_INTRO"));
	}
}

void UDoomWorld::PlayMusic(const FString& Lump)
{
	if (Lump == CurrentMusic && MusicComponent)
	{
		return; // same song keeps playing, like Doom
	}
	StopMusic();
	const float Volume = GetDefault<UDoomSettings>()->MusicVolume;
	if (Lump.IsEmpty() || !Wad.IsValid() || Volume <= 0.f || FParse::Param(FCommandLine::Get(), TEXT("nomusic")))
	{
		return;
	}
	TSharedPtr<FDoomMusicPlayer> Player = DoomMusic::CreatePlayer(*Wad, Lump, true);
	if (!Player)
	{
		return;
	}
	Player->Volume = Volume;
	MusicWave = NewObject<UDoomMusicWave>(this);
	MusicWave->Start(Player);
	// a 2D, non-spatialized component that survives map changes inside this world
	MusicComponent = UGameplayStatics::CreateSound2D(GetWorld(), MusicWave, 1.f, 1.f, 0.f, nullptr, true, false);
	if (MusicComponent)
	{
		MusicComponent->bIsMusic = true;
		MusicComponent->Play();
		CurrentMusic = Lump;
	}
}

void UDoomWorld::ShowMessage(const FString& Msg)
{
	Message = Msg;
	MessageTics = 4 * Doom::TicRate;
}

// ------------------------------------------------------------------------------------------------
// Tic
// ------------------------------------------------------------------------------------------------

void UDoomWorld::UpdateMaterialGlobals(float SubTic)
{
	// per-frame material globals (UMaterialParameterCollection)
	if (UMaterialParameterCollection* MPC = DoomAssets::Collection())
	{
		ADoomPlayer* P = GetPlayer();
		const float Tic = Flow().LevelTic + SubTic;
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("Tic"), Tic);
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("ExtraLight"), P ? P->ExtraLight : 0);
		float Fixed = -1.f;
		if (P)
		{
			if (P->Invulnerability > 4 * 32 || (P->Invulnerability & 8)) Fixed = 32.f;
			else if (P->LightAmp > 4 * 32 || (P->LightAmp & 8)) Fixed = 1.f;
		}
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("FixedColormap"), Fixed);
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("Modern"), bModern ? 1.f : 0.f);
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("Palette"), IsPaletteExact() ? ScreenPalette() : 0);
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("PaletteRows"), Gfx->ColormapRows());
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("PaletteExact"),
			(GetDefault<UDoomSettings>()->bPaletteExact && !FParse::Param(FCommandLine::Get(), TEXT("smooth"))) ? 1.f : 0.f);
	}
}

void UDoomWorld::RunTic()
{
	++Flow().LevelTic;
	if (MessageTics > 0) --MessageTics;

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
		if (Things[i].IsValid()) Things[i]->DoomTic();
		else Things.RemoveAtSwap(i);
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
				if (Mon.IsValid() && Mon->IsCorpseAsleep() && Map->SectorAt(Mon->DoomPos()) == M.Sector) Mon->WakeCorpse();
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
	M.bBlaze = Speed > DoorSpeed;
	M.Wait = DoorWait;
	M.High = Map->LowestCeilingSurrounding(Sector) - 4.f;
	const FVector Pos = Doom::ToUE(S.Center, (S.Floor + S.Ceiling) * 0.5f);
	switch (Kind)
	{
	case DK_Raise:
		M.bReturn = true; M.Dir = 1; M.Dest = M.High;
		if (S.Ceiling >= M.High) { M.Dir = 0; M.Counter = M.Wait; }
		PlaySound(M.bBlaze ? TEXT("BDOPN") : TEXT("DOROPN"), &Pos);
		break;
	case DK_Open:
		M.Dir = 1; M.Dest = M.High;
		if (S.Ceiling >= M.High) return;
		PlaySound(M.bBlaze ? TEXT("BDOPN") : TEXT("DOROPN"), &Pos);
		break;
	case DK_Close:
		M.Dir = -1; M.Dest = S.Floor;
		PlaySound(M.bBlaze ? TEXT("BDCLS") : TEXT("DORCLS"), &Pos);
		break;
	case DK_Close30:
		M.Dir = -1; M.Dest = S.Floor; M.CloseThenOpenWait = 30 * Doom::TicRate; M.High = S.Ceiling;
		PlaySound(TEXT("DORCLS"), &Pos);
		break;
	case DK_CloseIn30:
		// sector 10 (P_SpawnDoorCloseIn30): a normal door, open now, that closes after 30 s; blocked, it
		// reopens, waits and closes again like any door. No sound at load.
		M.bReturn = true; M.Dir = 0; M.Counter = 30 * Doom::TicRate; M.High = S.Ceiling;
		break;
	case DK_RaiseIn5Min:
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
	M.Wait = PlatWait;
	const FVector Pos = Doom::ToUE(S.Center, S.Floor);
	switch (Kind)
	{
	case PK_Lift:
		M.Low = FMath::Min(Map->LowestFloorSurrounding(Sector), S.Floor);
		M.High = S.Floor;
		M.Dir = -1; M.Dest = M.Low; M.bReturn = true;
		PlaySound(TEXT("PSTART"), &Pos);
		break;
	case PK_Perpetual:
		M.Low = FMath::Min(Map->LowestFloorSurrounding(Sector), S.Floor);
		M.High = FMath::Max(Map->HighestFloorSurrounding(Sector), S.Floor);
		M.bPerpetual = true;
		M.Dir = (DoomInfo::Random() & 1) ? 1 : -1;
		M.Dest = M.Dir > 0 ? M.High : M.Low;
		PlaySound(TEXT("PSTART"), &Pos);
		break;
	case PK_RaiseChange:
		M.Low = S.Floor; M.High = S.Floor + Amount;
		M.Dir = 1; M.Dest = M.High; M.Speed = 0.5f; M.bSilent = false;
		break;
	case PK_RaiseNextChange:
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
	case FK_RaiseLowestCeil: M.Dir = 1; M.Dest = FMath::Min(Map->LowestCeilingSurrounding(Sector), S.Ceiling); break;
	case FK_RaiseCrush: M.Dir = 1; M.Dest = FMath::Min(Map->LowestCeilingSurrounding(Sector), S.Ceiling) - 8; M.bCrush = true; break;
	case FK_RaiseNext: M.Dir = 1; M.Dest = Map->NextHighestFloor(Sector, S.Floor); break;
	case FK_LowerHighest: M.Dir = -1; M.Dest = Map->HighestFloorSurrounding(Sector); break;
	case FK_LowerHighestPlus8:
		M.Dir = -1; M.Dest = Map->HighestFloorSurrounding(Sector);
		if (M.Dest != S.Floor) M.Dest += 8;
		break;
	case FK_LowerLowest: M.Dir = -1; M.Dest = Map->LowestFloorSurrounding(Sector); break;
	case FK_LowerLowestChange:
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
	case FK_Raise24: M.Dir = 1; M.Dest = S.Floor + 24; break;
	case FK_Raise24Change:
		M.Dir = 1; M.Dest = S.Floor + 24;
		// raiseFloor24AndChange changes the texture and special as it starts
		if (LineIndex >= 0 && Map->Lines[LineIndex].FrontSector != INDEX_NONE)
		{
			S.FloorPic = Map->Sectors[Map->Lines[LineIndex].FrontSector].FloorPic;
			S.Special = Map->Sectors[Map->Lines[LineIndex].FrontSector].Special;
			Level->SectorChanged(Sector);
		}
		break;
	case FK_Raise512: M.Dir = 1; M.Dest = S.Floor + 512; break;
	case FK_RaiseShortest:
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
	case CK_RaiseHighest: M.Dir = 1; M.Dest = Map->HighestCeilingSurrounding(Sector); break;
	case CK_LowerFloor: M.Dir = -1; M.Dest = S.Floor; break;
	case CK_Lower8: M.Dir = -1; M.Dest = S.Floor + 8; break;
	case CK_Crusher:
	case CK_CrusherSilent:
		M.Type = FSectorMover::EType::Crusher;
		M.High = S.Ceiling; M.Low = S.Floor + 8; M.Dir = -1; M.Dest = M.Low; M.bCrush = true;
		M.bSilent = Kind == CK_CrusherSilent;
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
		case 10: AddDoor(i, DK_CloseIn30, DoorSpeed, nullptr); break;
		case 12: AddLight(FLightEffect::EType::StrobeSlow, 35, true); break;
		case 13: AddLight(FLightEffect::EType::StrobeFast, 15, true); break;
		case 14: AddDoor(i, DK_RaiseIn5Min, DoorSpeed, nullptr); break;
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
	const FSpec* Spec = FindSpec(L.Special);
	if (!Spec)
	{
		return false;
	}
	const bool bMonster = Activator && Activator->IsA<ADoomMonster>();
	if (bMonster && !Spec->bMonster && !(How == EActivation::Shoot && L.Special == 46))
	{
		return false; // (a monster's bullet may open the GR door 46, as in P_ShootSpecialLine)
	}
	if (!bMonster && Spec->Act == EAct::TeleportMonsterOnly)
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
	else if (Spec->Act != EAct::Exit && Spec->Act != EAct::SecretExit && Spec->Act != EAct::Teleport && Spec->Act != EAct::TeleportMonsterOnly)
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
	case EAct::DoorRaise: Door(DK_Raise, DoorSpeed); break;
	case EAct::DoorOpen: Door(DK_Open, DoorSpeed); break;
	case EAct::DoorClose: Door(DK_Close, DoorSpeed); break;
	case EAct::DoorClose30: Door(DK_Close30, DoorSpeed); break;
	case EAct::DoorBlazeRaise: Door(DK_Raise, DoorSpeed * 4); break;
	case EAct::DoorBlazeOpen: Door(DK_Open, DoorSpeed * 4); break;
	case EAct::DoorBlazeClose: Door(DK_Close, DoorSpeed * 4); break;
	case EAct::Lift: ForFree([&](int32 S) { AddPlat(S, PK_Lift, 4, 0); }); break;
	case EAct::LiftBlaze: ForFree([&](int32 S) { AddPlat(S, PK_Lift, 8, 0); }); break;
	case EAct::PlatPerpetual:
		for (FSectorMover& M : Movers)
		{
			if (M.bPerpetual && M.bStasis && Targets.Contains(M.Sector)) { M.bStasis = false; bDid = true; }
		}
		ForFree([&](int32 S) { AddPlat(S, PK_Perpetual, 1, 0); }); // PLATSPEED
		break;
	case EAct::PlatStop:
		for (FSectorMover& M : Movers)
		{
			if (M.bPerpetual && Targets.Contains(M.Sector)) { M.bStasis = true; bDid = true; }
		}
		break;
	case EAct::PlatRaise24Change:
	case EAct::PlatRaise32Change:
	case EAct::PlatRaiseNextChange:
		ForFree([&](int32 S)
		{
			if (Spec->Act == EAct::PlatRaiseNextChange) AddPlat(S, PK_RaiseNextChange, 0.5f, 0);
			else AddPlat(S, PK_RaiseChange, 0.5f, Spec->Act == EAct::PlatRaise24Change ? 24 : 32);
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
	case EAct::FloorRaiseLowestCeil: ForFree([&](int32 S) { AddFloor(S, FK_RaiseLowestCeil, 1, LineIndex); }); break;
	case EAct::FloorRaiseCrush: ForFree([&](int32 S) { AddFloor(S, FK_RaiseCrush, 1, LineIndex); }); break;
	case EAct::FloorRaiseNext: ForFree([&](int32 S) { AddFloor(S, FK_RaiseNext, 1, LineIndex); }); break;
	case EAct::FloorRaiseNextTurbo: ForFree([&](int32 S) { AddFloor(S, FK_RaiseNext, 4, LineIndex); }); break;
	case EAct::FloorLowerHighest: ForFree([&](int32 S) { AddFloor(S, FK_LowerHighest, 1, LineIndex); }); break;
	case EAct::FloorLowerHighestTurbo: ForFree([&](int32 S) { AddFloor(S, FK_LowerHighestPlus8, 4, LineIndex); }); break;
	case EAct::FloorLowerLowest: ForFree([&](int32 S) { AddFloor(S, FK_LowerLowest, 1, LineIndex); }); break;
	case EAct::FloorLowerLowestChange: ForFree([&](int32 S) { AddFloor(S, FK_LowerLowestChange, 1, LineIndex); }); break;
	case EAct::FloorRaise24: ForFree([&](int32 S) { AddFloor(S, FK_Raise24, 1, LineIndex); }); break;
	case EAct::FloorRaise24Change: ForFree([&](int32 S) { AddFloor(S, FK_Raise24Change, 1, LineIndex); }); break;
	case EAct::FloorRaise512: ForFree([&](int32 S) { AddFloor(S, FK_Raise512, 1, LineIndex); }); break;
	case EAct::FloorRaiseShortest: ForFree([&](int32 S) { AddFloor(S, FK_RaiseShortest, 1, LineIndex); }); break;
	case EAct::Donut:
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
	case EAct::CeilRaiseHighest: ForFree([&](int32 S) { AddCeiling(S, CK_RaiseHighest, 1); }); break;
	case EAct::CeilLowerFloor: ForFree([&](int32 S) { AddCeiling(S, CK_LowerFloor, 1); }); break;
	case EAct::CeilLower8: ForFree([&](int32 S) { AddCeiling(S, CK_Lower8, 1); }); break;
	case EAct::Crusher:
	case EAct::CrusherFast:
	case EAct::CrusherSilent:
		for (FSectorMover& M : Movers)
		{
			if (M.Type == FSectorMover::EType::Crusher && M.bStasis && Targets.Contains(M.Sector)) { M.bStasis = false; bDid = true; }
		}
		ForFree([&](int32 S)
		{
			AddCeiling(S, Spec->Act == EAct::CrusherSilent ? CK_CrusherSilent : CK_Crusher, Spec->Act == EAct::CrusherFast ? 2.f : 1.f);
		});
		break;
	case EAct::CrusherStop:
		for (FSectorMover& M : Movers)
		{
			if (M.Type == FSectorMover::EType::Crusher && Targets.Contains(M.Sector)) { M.bStasis = true; bDid = true; }
		}
		break;
	case EAct::Stairs8: for (int32 S : Targets) if (!SectorHasMover(S)) { BuildStairs(S, 8, 0.25f); bDid = true; } break;
	case EAct::Stairs16: for (int32 S : Targets) if (!SectorHasMover(S)) { BuildStairs(S, 16, 4); bDid = true; } break;
	case EAct::LightMaxNeighbour:
		for (int32 S : Targets) { Map->Sectors[S].Light = Map->MaxSurroundingLight(S, 0); }
		bDid = bLightsDirty = true;
		break;
	case EAct::LightMinNeighbour:
		for (int32 S : Targets) { Map->Sectors[S].Light = Map->MinSurroundingLight(S, Map->Sectors[S].Light); }
		bDid = bLightsDirty = true;
		break;
	case EAct::Light255: for (int32 S : Targets) Map->Sectors[S].Light = 255; bDid = bLightsDirty = true; break;
	case EAct::Light35: for (int32 S : Targets) Map->Sectors[S].Light = 35; bDid = bLightsDirty = true; break;
	case EAct::LightStrobe:
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
	case EAct::Teleport:
	case EAct::TeleportMonsterOnly:
		if (SideIdx == 0)
		{
			Teleport(Activator, LineIndex, SideIdx);
			bDid = true;
		}
		break;
	case EAct::Exit:
		if (Activator && Activator->IsA<ADoomPlayer>()) { if (How == EActivation::Use) FlipSwitch(LineIndex, SideIdx, false); ExitLevel(false); return true; }
		break;
	case EAct::SecretExit:
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
			const FSpec* S = FindSpec(L.Special);
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
		const FSpec* S = FindSpec(L.Special);
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
			const FSpec* S = FindSpec(L.Special);
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
			UE_LOG(LogDoom, Log, TEXT("DEHACKED lump: %s"), *DoomDehacked::Apply(Text).ToString());
		}
		else
		{
			UE_LOG(LogDoom, Log, TEXT("UMAPINFO lump: %d maps"), MapInfos.Parse(Text));
		}
	}
	for (const FString& File : DehFiles)
	{
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *File))
		{
			UE_LOG(LogDoom, Log, TEXT("%s: %s"), *FPaths::GetCleanFilename(File), *DoomDehacked::Apply(Text).ToString());
		}
		else
		{
			UE_LOG(LogDoom, Error, TEXT("Cannot read DeHackEd patch %s"), *File);
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
			UE_LOG(LogDoom, Log, TEXT("Nightmare: %s respawned"), *Risen->Def->Name);
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
					UE_LOG(LogDoom, Log, TEXT("Boss action: special %d on tag %d"), A.Special, A.Tag);
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
		Tagged(666, [&](int32 S) { AddDoor(S, DK_Open, DoorSpeed, nullptr); });
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
		if (Type == TEXT("Mancubus") && AllDead(TEXT("Mancubus"))) Tagged(666, [&](int32 S) { AddFloor(S, FK_LowerLowest, 1, -1); });
		if (Type == TEXT("Arachnotron") && AllDead(TEXT("Arachnotron"))) Tagged(667, [&](int32 S) { AddFloor(S, FK_RaiseShortest, 1, -1); });
		return;
	}
	if (Flow().CurrentMap == TEXT("E1M8") && Type == TEXT("BaronOfHell") && AllDead(TEXT("BaronOfHell")))
	{
		Tagged(666, [&](int32 S) { AddFloor(S, FK_LowerLowest, 1, -1); });
	}
	else if ((Flow().CurrentMap == TEXT("E2M8") && Type == TEXT("Cyberdemon")) || (Flow().CurrentMap == TEXT("E3M8") && Type == TEXT("SpiderMastermind")))
	{
		if (AllDead(*Type)) ExitLevel(false);
	}
	else if (Flow().CurrentMap == TEXT("E4M6") && Type == TEXT("Cyberdemon") && AllDead(TEXT("Cyberdemon")))
	{
		Tagged(666, [&](int32 S) { AddDoor(S, DK_Open, DoorSpeed * 4, nullptr); });
	}
	else if (Flow().CurrentMap == TEXT("E4M8") && Type == TEXT("SpiderMastermind") && AllDead(TEXT("SpiderMastermind")))
	{
		Tagged(666, [&](int32 S) { AddFloor(S, FK_LowerLowest, 1, -1); });
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
		UE_LOG(LogDoom, Log, TEXT("Client: building %s (level %d) from the server"), *F.CurrentMap, F.LevelSerial);
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
			Players += FString::Printf(TEXT(" [P%d hp%d am%d w%d k%d f%d]"), PS->PlayerNumber + 1, PS->Inv.Health, PS->Inv.Ammo[0],
				(int32)PS->Inv.Weapons[(int32)EDoomWeapon::Shotgun], PS->LevelKills, PS->Frags);
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
		const FSpec* S = L.Special ? FindSpec(L.Special) : nullptr;
		if (!S)
		{
			continue;
		}
		ESectorRole Role = ESectorRole::None;
		switch (S->Act)
		{
		case EAct::DoorRaise: case EAct::DoorOpen: case EAct::DoorBlazeRaise: case EAct::DoorBlazeOpen: case EAct::DoorClose30:
			Role = ESectorRole::Door; break;
		case EAct::Lift: case EAct::LiftBlaze: case EAct::PlatPerpetual:
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
		const FSpec* S = L.Special ? FindSpec(L.Special) : nullptr;
		if (!S || S->Act != EAct::Teleport)
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

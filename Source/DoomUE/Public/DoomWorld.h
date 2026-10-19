// UDoomWorld: the game simulation as a world subsystem. Owns the WAD, graphics,
// sound and map, builds the level, spawns things, and runs one Doom tic when
// ADoomGameMode's 35 Hz clock asks for it: sector movers (doors, lifts, floors,
// ceilings, crushers, stairs), light effects, linedef and sector specials, monsters
// and projectiles. The level's flow and totals live in ADoomGameState, the
// inventory in ADoomPlayerState (see DoomGameState.h for the division of work).
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DoomMap.h"
#include "DoomMapGen.h"
#include "DoomAI.h"
#include "DoomInfo.h"
#include "DoomWad.h"
#include "DoomGraphics.h"
#include "DoomSound.h"
#include "DoomMapInfo.h"
#include "DoomMenu.h"
#include "DoomAssets.h"
#include "DoomGameState.h"
#include "DoomWorld.generated.h"

class ADoomLevel;
class ADoomThing;
class ADoomMonster;
class ADoomProjectile;
class ADoomPlayer;
class APostProcessVolume;
class ADoomPlayerState;

enum class EActivation : uint8 { Use, Cross, Shoot };

struct FSectorMover
{
	enum class EType : uint8 { Door, Plat, Floor, Ceiling, Crusher };
	EType Type = EType::Floor;
	int32 Sector = 0;
	bool bCeiling = false;      // which plane moves
	float Speed = 1;            // units per tic
	int32 Dir = 1;              // +1 up, -1 down, 0 waiting
	float Dest = 0;
	// doors / plats / crushers
	float Low = 0, High = 0;
	int32 Wait = 0, Counter = 0;
	bool bReturn = false;       // door raise / plat: come back after waiting
	bool bPerpetual = false;
	bool bStasis = false;
	bool bCrush = false;
	bool bSilent = false;
	bool bBlaze = false;
	int32 Tag = 0;
	int32 CloseThenOpenWait = 0; // door close-30-then-open
	FName ChangeFloorPic;       // floor texture change on arrival
	int16 ChangeSpecial = -1;
	bool bDone = false;
};

struct FLightEffect
{
	enum class EType : uint8 { Flicker, StrobeFast, StrobeSlow, Glow, FireFlicker };
	EType Type;
	int32 Sector;
	int32 Min, Max;
	int32 Count = 0;
	int32 DarkTime = 0, BrightTime = 0;
	int32 Dir = -1;
};

struct FSwitchButton
{
	int32 Line;
	int32 Side;
	int32 Part; // 0 upper 1 middle 2 lower
	FName Original;
	int32 Timer;
};

/** WADs and patches to load instead of the settings and the command line (automation tests). */
struct FDoomWadSetup
{
	/** File name in Content/WADs or a path; empty: the settings' IWAD. */
	FString IWad;
	TArray<FString> PWads;
	TArray<FString> DehFiles;
};

UCLASS()
class DOOMUE_API UDoomWorld : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UDoomWorld();
	virtual ~UDoomWorld() override;

	// USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	// FTickableGameObject: on servers and standalone ADoomGameMode::Tick runs the clock; network
	// clients run no game tics, only ClientTick (texture animation, replicated geometry, globals);
	// everyone turns the sleeping corpse sprites to the camera
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UDoomWorld, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return Wad.IsValid() && (IsNetClient() || SleepingSprites.Num() > 0); }
	/** A corpse's sprite that stopped ticking (UDoomSpriteComponent::bSleepWhenDone). */
	void AddSleepingSprite(class UDoomSpriteComponent* Sprite);
	int32 NumSleepingSprites() const { return SleepingSprites.Num(); }

	// --- networking ---------------------------------------------------------------------------
	bool IsNetClient() const;
	bool IsNetServer() const;
	/** Any networked game (co-op or deathmatch, server or client). */
	bool IsNetGame() const;
	bool IsDeathmatch() const { return Flow().bDeathmatch; }
	/** Loads the WADs if they are not loaded yet (clients load them on demand). */
	bool EnsureWads() { return Wad.IsValid() || InitWads(); }
	/** Client: the game state's flow record arrived (a new map to build, a phase change). */
	void OnFlowReplicated();
	/** Client: a sector or sidedef changed on the server. */
	void ApplySectorNet(const struct FDoomSectorNet& Net);
	void ApplySideNet(const struct FDoomSideNet& Net);

	// --- players ------------------------------------------------------------------------------
	/** Every player pawn in the world (the local one first on a listen server). */
	TArray<ADoomPlayer*> GetPlayers() const;
	/** The living player closest to From (optionally only those in sight), or null. */
	ADoomPlayer* NearestPlayer(const AActor* From, bool bInSight) const;
	/** Moves a player to their start: co-op start by player number, or a random deathmatch start. */
	void PlacePlayer(ADoomPlayer* P);

	/** Sectors that doors, lifts, floors, stairs and sector specials can move: each gets its own
	    geometry chunk; the rest is the static chunk (also used by the editor preview and tile bake). */
	static void ComputeMovableSectors(const FDoomMap& Map, TSet<int32>& Out);

	/** What a sector does in the level, from the specials that move it (map exporters). */
	enum class ESectorRole : uint8 { None, Door, Lift };
	static void ClassifySectors(const FDoomMap& Map, TArray<ESectorRole>& Out);
	/** Teleporter lines (walk-over, players) and the teleport destination thing each leads to. */
	static void FindTeleporters(const FDoomMap& Map, TArray<TPair<int32, int32>>& OutLineAndThing);

	/** -nettest: the sector behind the map's first manual door (both sides find the same one). */
	int32 NetTestDoorSector() const;
	/** -nettest on the server: open that door. */
	void NetTestOpenDoor();
	/** A line of what this machine sees, for comparing server and client logs. */
	FString NetFingerprint() const;

	// --- the clock (driven by ADoomGameMode) --------------------------------------------------
	/** One 35 Hz game tic: the player, monsters, things, missiles, movers, lights, specials. */
	void RunTic();
	/** One tic of the story text / ending screens. */
	void TicFinale();
	/** Per-frame material globals; SubTic (0..1) interpolates the animated ones between tics. */
	void UpdateMaterialGlobals(float SubTic);
	/** Restarts the game mode's tic accumulator (new map, loaded game). */
	void ResetClock();

	// --- level flow and totals (ADoomGameState) ---------------------------------------------
	/** The game state's flow record (a local one when the world has no Doom game state). */
	FDoomLevelFlow& Flow();
	const FDoomLevelFlow& Flow() const;
	class ADoomGameState* GetDoomGameState() const;
	/** Counts a kill / item / secret for the level and for the player who earned it. */
	void CreditKill(AActor* Killer);
	/** The player state behind an actor: a player, their controller, or a missile they fired. */
	static ADoomPlayerState* CreditedPlayer(AActor* Actor);
	/** Who to blame for damage done by DamageCauser (vanilla's "source"): a missile's shooter, the player
	 *  or monster whose shot set a barrel off, a controller's pawn, else the causer itself. */
	static AActor* DamageSource(AActor* DamageCauser);
	void CreditItem(ADoomPlayer* Player);
	void CreditSecret(ADoomPlayer* Player);

	/** Doom's -deathmatch, read when the game boots. */
	bool bDeathmatchParam = false;

	/** Loads the configured IWAD (+ PWADs). */
	bool InitWads(const FDoomWadSetup* Setup = nullptr);

	/** Loads a map and places the player. Inventory carries over unless bPistolStart. */
	bool LoadMap(const FString& MapName, bool bPistolStart, bool bForSaveGame = false);
	/** Loads a map the map editor made or changed (its name stays; the level's own edits are not
	 *  applied again): the level, things and players start afresh from it. */
	bool LoadEditedMap(const FDoomMap& Edited);
	/**
	 * Starts loading a map behind the loading screen and returns at once. The map's pictures are
	 * decoded and its geometry generated on worker threads, and the game thread creates textures
	 * and commits mesh chunks a few milliseconds per frame (ADoomGameMode::Tick calls StepLoad).
	 * LoadMap runs the same steps without returning in between.
	 */
	bool LoadMapAsync(const FString& MapName, bool bPistolStart);
	bool IsLoadingMap() const { return PendingLoad.IsValid(); }
	/** Advances a pending load until Deadline (FPlatformTime::Seconds); true once the map is playing. */
	bool StepLoad(double Deadline);
	/** 0..1, for the loading screen. */
	float LoadingProgress() const;

	// --- save games (DoomSave.cpp) ------------------------------------------------------------
	/** Slot 0 = quicksave, 1..6 = menu slots. */
	bool SaveGame(int32 Slot, const FString& Name = FString());
	bool LoadGame(int32 Slot);
	FString SlotDescription(int32 Slot);
	void RefreshSlots();
	static FString SlotName(int32 Slot);
	/** Saves, scrambles the world, loads and compares (-savetest, DoomUE.Game.SaveLoad). Logs, and
	 *  returns whether the loaded state matches the saved one; OutReport gets the three fingerprints. */
	bool RunSaveTest(FString* OutReport);
	FString StateFingerprint() const;
	int32 NumMovers() const { return Movers.Num(); }
	/** A door, lift, floor or crusher is working on the sector (a corpse there stays awake). */
	bool IsSectorMoving(int32 Sector) const { return SectorHasMover(Sector); }
	void UnloadMap();

	/** Level flow: forwards to ADoomGameMode, which owns exits, intermission and the next map. */
	void ExitLevel(bool bSecret);
	void FinishIntermission();
	/** Leaves the story text screen (UMAPINFO intertext). */
	void FinishFinale();
	FString NextMapName(bool bSecret) const;

	/** UMAPINFO entry for a map (null if none). */
	const FDoomMapInfo* MapInfo(const FString& MapName) const { return MapInfos.Find(MapName); }
	/** "E1M1: Hangar": UMAPINFO label and name, else the WAD's DEHACKED level name, else the lump. */
	FString LevelTitle(const FString& MapName) const;
	/** The music lump for a map (a generated map borrows one of the IWAD's songs). */
	FString MusicForMap(const FString& MapName) const;
	/** The generator's options for a seed: this IWAD's game, -genrooms. */
	DoomMapGen::FOptions GenOptions(uint32 Seed) const;
	/** Runs a linedef special on a tag as if a line had been triggered (UMAPINFO boss actions). */
	bool ExecuteSpecial(int32 Special, int32 Tag);

	/** Story text screen and endings (EDoomGameState::Finale; DoomFinale.cpp). */
	enum class EFinaleStage : uint8 { Text, Picture, Bunny, Cast };
	EFinaleStage FinaleStage = EFinaleStage::Text;
	TArray<FString> FinaleText;
	FString FinaleBackdrop;
	FString FinalePicture;
	int32 FinaleTic = 0;
	/** Enter / fire on a finale screen: finish typing, go on, or kill the cast member. */
	void FinaleInput();
	bool IsFinaleTextComplete() const;

	/** Doom 2 cast call. */
	struct FCastState
	{
		int32 Index = 0;
		FString Sequence;
		int32 Frame = 0;
		int32 FrameTics = 0;
		int32 Walked = 0;
		bool bAttacking = false;
		bool bDying = false;
		int32 DeadTics = 0;
	} CastCall;
	FString CastSprite() const;
	FString CastName() const;

	/** Test runs: ignore all player input (-autotest). */
	bool bInputLocked = false;

	// --- queries ----------------------------------------------------------------------------------
	FDoomMap* GetMap() const { return Map.Get(); }
	FDoomGraphics* GetGraphics() const { return Gfx.Get(); }
	FDoomWad* GetWad() const { return Wad.Get(); }
	ADoomLevel* GetLevel() const { return Level; }
	ADoomPlayer* GetPlayer() const;
	int32 GetTic() const { return Flow().LevelTic; }
	int32 GetSkill() const { return Skill; }
	bool IsModern() const { return bModern; }
	/** Monster AI: SMART (Unreal's AI framework picks where monsters go, DoomAI.h) or CLASSIC. */
	bool IsSmartAI() const { return bSmartAI; }
	/** Switches every monster now and the ones spawned later. */
	void SetSmartAI(bool bOn);
	/** Monster paths over the current map (built the first time they're asked for). */
	const FDoomNav& GetNav() const;
	/** Niagara bursts spawned so far (tests). */
	int32 GetParticleBursts() const { return ParticleBursts; }
	/** The phase this machine shows: the game state's, or Menu while this player's menu is open in a
	    network game (where the menu never pauses the shared game). */
	EDoomGameState GetState() const { return bNetMenu ? EDoomGameState::Menu : Flow().Phase; }
	void SetState(EDoomGameState S);
	const FString& GetMapName() const { return Flow().CurrentMap; }

	int32 SectorAt(const FVector& UEPos) const;
	float FloorAt(const FVector& UEPos) const;
	float CeilingAt(const FVector& UEPos) const;

	// --- actions ----------------------------------------------------------------------------------
	void PlaySound(const FString& Name, const FVector* Location, float Volume = 1.f);
	/** This machine only (PlaySound also sends it to the clients). */
	void PlaySoundLocal(const FString& Name, const FVector* Location, float Volume = 1.f);
	/** Starts a music lump (D_E1M1, D_RUNNIN...) looping; empty or missing stops the music. */
	void PlayMusic(const FString& Lump);
	void PlayTitleMusic();
	void StopMusic();
	void PlaySoundAt(const FString& Name, const FVector& Location) { PlaySound(Name, &Location); }

	/** Player pressed use: P_UseLines (64 units ahead). */
	void UseLines(AActor* User, const FVector& From, float DoomAngle);
	/** An actor moved from A to B (Doom 2D coordinates): walk-over triggers. */
	void CheckCrossings(AActor* Actor, const FVector2f& From, const FVector2f& To, bool bMonster);
	/** A hitscan hit a wall: gun-activated lines between From and To. */
	void CheckShootLines(AActor* Shooter, const FVector2f& From, const FVector2f& To);
	/** Wake monsters that can hear the noise (P_NoiseAlert). */
	void NoiseAlert(AActor* Target, const FVector& Where);
	bool TryMonsterUse(ADoomMonster* Monster, const FVector2f& From, const FVector2f& To);

	ADoomThing* SpawnThing(int32 DoomEdNum, const FVector& UEFeet, float DoomAngle, bool bDropped = false);
	ADoomThing* SpawnEffect(const FString& Sprite, const FString& Frames, int32 Tics, const FVector& UEPos, bool bFullBright = true, bool bLoop = false);
	ADoomThing* SpawnEffectLocal(const FString& Sprite, const FString& Frames, int32 Tics, const FVector& UEPos, bool bFullBright = true, bool bLoop = false);
	/** Modern renderer only: a Niagara burst on top of Doom's sprite effect, sprayed around Up. */
	void SpawnParticles(EDoomParticles Kind, const FVector& UEPos, const FVector& Up = FVector::UpVector);
	void SpawnParticlesLocal(EDoomParticles Kind, const FVector& UEPos, const FVector& Up);
	ADoomProjectile* SpawnProjectile(const FString& Type, AActor* Source, const FVector& From, const FVector& Direction, AActor* Tracer = nullptr, bool bSilent = false);
	ADoomMonster* SpawnMonster(int32 DoomEdNum, const FVector& UEFeet, float DoomAngle);
	void RadiusAttack(AActor* Source, AActor* Instigator, const FVector& Where, float Damage);
	bool CheckSight(const AActor* From, const AActor* To) const;

	/** Kill bookkeeping and the boss-death map events (A_BossDeath, A_KeenDie). */
	void OnMonsterDied(ADoomMonster* M);

	void RegisterThing(ADoomThing* T);
	void RegisterMonster(ADoomMonster* M);
	void RegisterProjectile(ADoomProjectile* P);
	const TArray<TWeakObjectPtr<ADoomMonster>>& GetMonsters() const { return Monsters; }
	const TArray<TWeakObjectPtr<ADoomThing>>& GetThings() const { return Things; }

	/** Message for the HUD (pickups, locked doors...). */
	void ShowMessage(const FString& Msg);
	FString Message;
	int32 MessageTics = 0;

	/** Automap: lines seen by the player, or all with the computer map. */
	bool bAutomap = false;
	bool bFullMap = false;

	// menu state (drawn by ADoomHUD, driven by ADoomPlayerController)
	/** Doom's menu (DoomMenu.cpp), drawn by ADoomHUD and driven by ADoomPlayerController. */
	FDoomMenu Menu;
	const FDoomMapInfoSet& GetMapInfos() const { return MapInfos; }
	/** Pushes volumes and field of view from UDoomSettings to the running game. */
	void ApplyUserSettings();
	/** Doom's screen palette for the player (0 normal, 1-8 red, 9-12 gold, 13 radiation suit). */
	int32 ScreenPalette() const;
	/** True when the classic renderer shows exact COLORMAP / PLAYPAL colours. */
	bool IsPaletteExact() const;
	void StartNewGame(const FString& MapName, int32 InSkill);

	void SetModern(bool b);

private:
	friend class ADoomGameMode; // the level flow (exits, intermission, next map) uses the internals below
	void SpawnMapThings();
	void StartSectorSpecials();
	void TicMovers();
	void TicLights();
	void TicSectorDamage();
	void TicSectorDamage(ADoomPlayer* P, int32 Sector);
	bool MovePlane(FSectorMover& M, bool& bBlocked);
	bool ActivateSpecial(int32 Line, int32 Side, AActor* Activator, EActivation How, bool bForce = false);
	bool CheckLock(AActor* Activator, int32 Lock);
	void FlipSwitch(int32 Line, int32 Side, bool bRepeat);
	bool SectorHasMover(int32 Sector) const;
	FSectorMover* FindMover(int32 Sector);
	void AddDoor(int32 Sector, int32 Kind, float Speed, AActor* Activator);
	void AddPlat(int32 Sector, int32 Kind, float Speed, int32 Amount);
	void AddFloor(int32 Sector, int32 Kind, float Speed, int32 Line);
	void AddCeiling(int32 Sector, int32 Kind, float Speed);
	void BuildStairs(int32 Sector, float StepSize, float Speed);
	void Teleport(AActor* Actor, int32 Line, int32 Side);
	bool ThingsBlockPlane(int32 Sector, float Floor, float Ceiling, bool bCrush);
	void SetupEnvironment();

	TUniquePtr<FDoomWad> Wad;
	TUniquePtr<FDoomGraphics> Gfx;
	TUniquePtr<FDoomSound> Sound;
	TUniquePtr<FDoomMap> Map;
	/** The map load in progress (LoadMapAsync), or null. */
	TSharedPtr<struct FDoomMapLoad> PendingLoad;
	bool BeginLoad(const FString& MapName, bool bPistolStart, bool bForSaveGame);
	void FinishLoad(const struct FDoomMapLoad& Load);

	UPROPERTY()
	TObjectPtr<ADoomLevel> Level;
	UPROPERTY()
	TArray<TObjectPtr<AActor>> EnvironmentActors;
	UPROPERTY()
	TObjectPtr<APostProcessVolume> PostProcess;
	UPROPERTY()
	TObjectPtr<class UAudioComponent> MusicComponent;
	UPROPERTY()
	TObjectPtr<class UDoomMusicWave> MusicWave;
	FString CurrentMusic;
	FString IWadName;
	/** The loaded WADs, as baked tile sets are named (UDoomMapTiles::WadSetName). */
	FString WadSetName;
	FDoomMapInfoSet MapInfos;

	// finales (DoomFinale.cpp)
	bool StartVanillaText();
	void StartFinaleText(const TArray<FString>& Lines, const FString& Backdrop);
	void StartEnding();
	void StartCastMember(int32 Index);
	void SetCastSequence(const FString& Frames);

	// the Icon of Sin's monster spawner (DoomFinale.cpp)
	struct FBrainCube
	{
		TWeakObjectPtr<ADoomThing> Visual;
		FVector Velocity;
		FVector Target;
		int32 TicsLeft = 0;
	};
	TArray<FVector> BrainShooters, BrainTargets;
	TArray<FBrainCube> BrainCubes;
	bool bBrainAwake = false;
	int32 BrainTimer = 0;
	int32 BrainTargetIndex = 0;
	int32 BrainExitTics = 0;
	void TicBrain();
	void TicRespawn();
	void LoadPatches(const TArray<FString>& DehFiles);
	TMap<int32, FString> SlotCache;
	void SerializeState(FArchive& Ar);
	/** After SerializeState loaded a level: geometry, lights and things follow the restored sectors. */
	void RefreshRestoredLevel();

	TArray<TWeakObjectPtr<ADoomThing>> Things;
	TArray<TWeakObjectPtr<ADoomMonster>> Monsters;
	/** Corpse sprites that face the camera from Tick instead of their own tick: one loop instead of a
	 *  tick function each, and only the ones rendered lately. */
	TArray<TWeakObjectPtr<class UDoomSpriteComponent>> SleepingSprites;
	TArray<TWeakObjectPtr<ADoomProjectile>> Projectiles;
	TArray<FSectorMover> Movers;
	TArray<FLightEffect> Lights;
	TArray<FSwitchButton> Buttons;
	bool bLightsDirty = false;

	FDoomLevelFlow LocalFlow; // only used without an ADoomGameState
	int32 Skill = 3;
	bool bModern = false;
	bool bSmartAI = false;
	TUniquePtr<FDoomMap> PendingEdited;
	mutable FDoomNav Nav;
	mutable bool bNavBuilt = false;
	bool bNoParticles = false; // -noparticles
	int32 ParticleBursts = 0;
	// networking
	void ClientTick(float DeltaTime);
	TArray<FDoomSector> BaselineSectors; // server: the level as loaded, to publish only changes
	TArray<FDoomSide> BaselineSides;
	TArray<FDoomMapThing> PlayerStarts;  // co-op starts 1-4 (Type 0 when the map has none)
	TArray<FDoomMapThing> DeathmatchStarts;
	int32 LoadedSerial = -1;             // client: the level serial its geometry was built for
	bool bClientGeometryDirty = false;
	double ClientAccumulator = 0;
	int32 ClientTic = 0;
	EDoomGameState LastClientPhase = EDoomGameState::Title;
	bool bNetMenu = false;
	int32 ValidCount = 0;
};

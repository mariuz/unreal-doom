// UDoomWorld: the game simulation as a world subsystem. Owns the WAD, graphics,
// sound and map, builds the level, spawns things, and runs Doom's 35 Hz tic:
// sector movers (doors, lifts, floors, ceilings, crushers, stairs), light effects,
// linedef and sector specials, monsters, projectiles and the level flow.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DoomMap.h"
#include "DoomInfo.h"
#include "DoomWad.h"
#include "DoomGraphics.h"
#include "DoomSound.h"
#include "DoomMapInfo.h"
#include "DoomMenu.h"
#include "DoomAssets.h"
#include "DoomWorld.generated.h"

class ADoomLevel;
class ADoomThing;
class ADoomMonster;
class ADoomProjectile;
class ADoomPlayer;
class APostProcessVolume;

enum class EDoomGameState : uint8 { Title, Menu, Playing, Intermission, Finale };

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
	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UDoomWorld, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return Wad.IsValid(); }

	/** Loads the configured IWAD (+ PWADs). */
	bool InitWads();

	/** Loads a map and places the player. Inventory carries over unless bPistolStart. */
	bool LoadMap(const FString& MapName, bool bPistolStart, bool bForSaveGame = false);

	// --- save games (DoomSave.cpp) ------------------------------------------------------------
	/** Slot 0 = quicksave, 1..6 = menu slots. */
	bool SaveGame(int32 Slot, const FString& Name = FString());
	bool LoadGame(int32 Slot);
	FString SlotDescription(int32 Slot);
	void RefreshSlots();
	static FString SlotName(int32 Slot);
	/** Saves, scrambles the world, loads and compares (-savetest). */
	void RunSaveTest();
	FString StateFingerprint() const;
	int32 NumMovers() const { return Movers.Num(); }
	void UnloadMap();

	/** Level flow. */
	void ExitLevel(bool bSecret);
	void FinishIntermission();
	/** Leaves the story text screen (UMAPINFO intertext). */
	void FinishFinale();
	FString NextMapName(bool bSecret) const;

	/** UMAPINFO entry for a map (null if none). */
	const FDoomMapInfo* MapInfo(const FString& MapName) const { return MapInfos.Find(MapName); }
	/** "E1M1: Hangar": UMAPINFO label and name, else the WAD's DEHACKED level name, else the lump. */
	FString LevelTitle(const FString& MapName) const;
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
	int32 GetTic() const { return LevelTic; }
	int32 GetSkill() const { return Skill; }
	bool IsModern() const { return bModern; }
	/** Niagara bursts spawned so far (tests). */
	int32 GetParticleBursts() const { return ParticleBursts; }
	EDoomGameState GetState() const { return State; }
	void SetState(EDoomGameState S) { State = S; }
	const FString& GetMapName() const { return CurrentMap; }

	int32 SectorAt(const FVector& UEPos) const;
	float FloorAt(const FVector& UEPos) const;
	float CeilingAt(const FVector& UEPos) const;

	// --- actions ----------------------------------------------------------------------------------
	void PlaySound(const FString& Name, const FVector* Location, float Volume = 1.f);
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
	/** Modern renderer only: a Niagara burst on top of Doom's sprite effect, sprayed around Up. */
	void SpawnParticles(EDoomParticles Kind, const FVector& UEPos, const FVector& Up = FVector::UpVector);
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

	// stats
	int32 TotalKills = 0, TotalItems = 0, TotalSecrets = 0;
	int32 Kills = 0, Items = 0, Secrets = 0;
	int32 IntermissionTic = 0;
	bool bSecretExit = false;
	FString PendingMap;

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
	void RunTic();
	void SpawnMapThings();
	void ComputeMovableSectors(TSet<int32>& Out) const;
	void StartSectorSpecials();
	void TicMovers();
	void TicLights();
	void TicSectorDamage();
	bool MovePlane(FSectorMover& M);
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
	FDoomMapInfoSet MapInfos;

	// finales (DoomFinale.cpp)
	bool StartVanillaText();
	void StartFinaleText(const TArray<FString>& Lines, const FString& Backdrop);
	void StartEnding();
	void TicFinale();
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

	TArray<TWeakObjectPtr<ADoomThing>> Things;
	TArray<TWeakObjectPtr<ADoomMonster>> Monsters;
	TArray<TWeakObjectPtr<ADoomProjectile>> Projectiles;
	TArray<FSectorMover> Movers;
	TArray<FLightEffect> Lights;
	TArray<FSwitchButton> Buttons;
	bool bLightsDirty = false;

	EDoomGameState State = EDoomGameState::Title;
	FString CurrentMap;
	int32 Skill = 3;
	bool bModern = false;
	bool bNoParticles = false; // -noparticles
	int32 ParticleBursts = 0;
	int32 LevelTic = 0;
	double Accumulator = 0;
	double ScreenAccumulator = 0;
	int32 ValidCount = 0;
};

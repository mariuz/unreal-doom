#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "DoomGameMode.generated.h"

/**
 * The rules of the game (server side in a networked game): boots the WADs, then shows the title
 * screen or warps to -warp= / StartMap; runs Doom's fixed 35 Hz clock (UDoomWorld simulates each
 * tic); and owns the level flow: exits, the intermission and the choice of the next map.
 * Uses ADoomGameState (level totals) and ADoomPlayerState (inventory); see DoomGameState.h.
 */
UCLASS()
class DOOMUE_API ADoomGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ADoomGameMode();
	virtual void StartPlay() override;
	/** The 35 Hz clock: whole tics of the level, the intermission or the story screens. */
	virtual void Tick(float DeltaSeconds) override;

	// --- level flow ---------------------------------------------------------------------------
	/** Leaves the level (exit line or special): the intermission, or straight on with UMAPINFO's nointermission. */
	void ExitLevel(bool bSecret);
	/** Leaves the intermission: story text, the ending, or the next map. */
	void FinishIntermission();
	/** The map an exit leads to (UMAPINFO next / nextsecret, else Doom's own order); empty at an episode's end. */
	FString NextMapName(bool bSecret) const;
	/** Starts counting tics afresh (new map, loaded game). */
	void ResetClock() { Accumulator = ScreenAccumulator = 0; }
	/** Screenshot tests: no tics run on their own (the test runs an exact number), while the engine
	 *  keeps rendering and updating the camera, which pausing the game would stop. */
	bool bClockStopped = false;
	/** Spawns the marine; once a map is loaded, at the player's start (LoadMap places everyone else). */
	virtual void RestartPlayer(AController* NewPlayer) override;
	/** Numbers joining players 1 to 4 (their start and status bar colour). */
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	/** Network games: a dead player is reborn at a start with a pistol while the level carries on. */
	void RespawnPlayer(AController* Controller);

	/** Console: DoomMap E1M2 */
	UFUNCTION(Exec)
	void DoomMap(const FString& MapName);

	/** Console: DoomEdit (the map editor, DoomEditor.h) */
	UFUNCTION(Exec)
	void DoomEdit();

	/** Console: DoomGod */
	UFUNCTION(Exec)
	void DoomGod();

	/** Console: DoomGiveAll (weapons, ammo, keys) */
	UFUNCTION(Exec)
	void DoomGiveAll();

private:
	class UDoomWorld* DoomWorld() const;
	/** -playtest: the scripted play-through of the interactive features (DoomPlayTest.cpp). */
	void StartPlayTest();
	double Accumulator = 0;       // level time not yet run as tics
	double ScreenAccumulator = 0; // same, for the intermission and story screens
	/** The menu's host page travel options: DoomMap, DoomRules (0 co-op, 1 deathmatch, 2 2.0),
	 *  NoMonsters, Fast. */
	FString OptMap;
	int32 OptRules = -1;
	bool bOptNoMonsters = false, bOptFast = false;
};

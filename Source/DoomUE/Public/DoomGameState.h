// ADoomGameState: the shared state of the level being played, replicated to every client.
//
// Unreal's gameplay framework splits a game into rules (AGameModeBase, server only), shared
// state (AGameStateBase, everyone), per-player state (APlayerState) and the pawn. The Doom port
// follows it:
//
//  * ADoomGameMode   the rules and the clock: the fixed 35 Hz tic, exits, intermission, next map.
//  * ADoomGameState  this file: phase (title, playing, intermission...), map, level tic and the
//                    kill / item / secret totals the intermission screen shows.
//  * ADoomPlayerState  each player's inventory and their own kills, items, secrets and frags.
//  * UDoomWorld      the simulation run once per tic: sectors, movers, lights, specials, monsters.
//
// The single-player game keeps one of each; co-op and deathmatch add player states, and the
// replicated properties here are what clients would read.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "DoomGameState.generated.h"

UENUM(BlueprintType)
enum class EDoomGameState : uint8 { Title, Menu, Playing, Intermission, Finale };

/** Where the game is in Doom's flow, and the level's running totals. */
USTRUCT(BlueprintType)
struct FDoomLevelFlow
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	EDoomGameState Phase = EDoomGameState::Title;

	/** The map being played (E1M1, MAP01...). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	FString CurrentMap;

	/** Tics since the level started (35 per second). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 LevelTic = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 Kills = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 Items = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 Secrets = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 TotalKills = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 TotalItems = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 TotalSecrets = 0;

	/** Intermission screen tics (its counters animate from this). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 IntermissionTic = 0;

	/** The map the exit leads to (empty: end of the episode), and whether it was the secret exit. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	FString PendingMap;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	bool bSecretExit = false;

	/** Clears the per-level counters for a new map. */
	void StartLevel(const FString& Map)
	{
		CurrentMap = Map;
		LevelTic = 0;
		Kills = Items = Secrets = 0;
		TotalKills = TotalItems = TotalSecrets = 0;
	}
};

UCLASS()
class DOOMUE_API ADoomGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	FDoomLevelFlow Flow;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};

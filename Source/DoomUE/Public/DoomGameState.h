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
// Networking (listen server or dedicated server, up to 4 players): only the server simulates.
// Clients build the map named by Flow, take the sectors and switch textures that changed from
// the fast arrays below, receive monsters, things and missiles as replicated actors, and get
// sounds and short effects through the multicasts.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "DoomGameState.generated.h"

class ADoomGameState;
class ADoomPlayerState;
class FDoomMap;
struct FDoomSector;
struct FDoomSide;

UENUM(BlueprintType, meta = (ScriptName = "DoomPhase"))
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

	/** Bumped by every map load, so clients rebuild (also on restarting the same map). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 LevelSerial = 0;

	/** Deathmatch rules: deathmatch starts, weapons stay, no keys. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	bool bDeathmatch = false;

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

/** A sector whose planes, light, flats or special differ from the WAD's (server to clients). */
USTRUCT()
struct FDoomSectorNet : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY() int32 LevelSerial = 0;
	UPROPERTY() int32 Sector = -1;
	UPROPERTY() float Floor = 0;
	UPROPERTY() float Ceiling = 0;
	UPROPERTY() int16 Light = 0;
	UPROPERTY() int16 Special = 0;
	UPROPERTY() FName FloorPic;
	UPROPERTY() FName CeilingPic;

	void PostReplicatedAdd(const struct FDoomSectorNetArray& Array);
	void PostReplicatedChange(const struct FDoomSectorNetArray& Array);
};

USTRUCT()
struct FDoomSectorNetArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY() TArray<FDoomSectorNet> Items;
	UPROPERTY(NotReplicated) TObjectPtr<ADoomGameState> Owner;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FDoomSectorNet, FDoomSectorNetArray>(Items, DeltaParms, *this);
	}
};

template<>
struct TStructOpsTypeTraits<FDoomSectorNetArray> : public TStructOpsTypeTraitsBase2<FDoomSectorNetArray>
{
	enum { WithNetDeltaSerializer = true };
};

/** A sidedef whose textures changed (switches, texture-changing specials). */
USTRUCT()
struct FDoomSideNet : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY() int32 LevelSerial = 0;
	UPROPERTY() int32 Side = -1;
	UPROPERTY() FName Upper;
	UPROPERTY() FName Middle;
	UPROPERTY() FName Lower;

	void PostReplicatedAdd(const struct FDoomSideNetArray& Array);
	void PostReplicatedChange(const struct FDoomSideNetArray& Array);
};

USTRUCT()
struct FDoomSideNetArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY() TArray<FDoomSideNet> Items;
	UPROPERTY(NotReplicated) TObjectPtr<ADoomGameState> Owner;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FDoomSideNet, FDoomSideNetArray>(Items, DeltaParms, *this);
	}
};

template<>
struct TStructOpsTypeTraits<FDoomSideNetArray> : public TStructOpsTypeTraitsBase2<FDoomSideNetArray>
{
	enum { WithNetDeltaSerializer = true };
};

UCLASS()
class DOOMUE_API ADoomGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	ADoomGameState();

	UPROPERTY(ReplicatedUsing = OnRep_Flow, VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	FDoomLevelFlow Flow;

	UPROPERTY(Replicated)
	FDoomSectorNetArray SectorNet;
	UPROPERTY(Replicated)
	FDoomSideNetArray SideNet;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Server: publish the sectors and sides that differ from the level's start (after each tic). */
	void PublishWorld(const FDoomMap& Map, const TArray<FDoomSector>& Baseline, const TArray<FDoomSide>& BaselineSides);
	/** Server: forget the previous level's changes. */
	void ResetWorldNet();

	/** Server to the clients: sounds, sprite effects (puffs, blood, fog) and particle bursts. */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastSound(const FString& Name, FVector_NetQuantize Location, bool bLocated, float Volume, APlayerState* Except);
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastEffect(const FString& Sprite, const FString& Frames, uint8 Tics, FVector_NetQuantize Location, bool bFullBright, bool bLoop);
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastParticles(uint8 Kind, FVector_NetQuantize Location, FVector_NetQuantizeNormal Up);

	/** Players in player-number order (player 1 first), for scoreboards. */
	TArray<ADoomPlayerState*> DoomPlayers() const;

private:
	UFUNCTION()
	void OnRep_Flow();
	TMap<int32, int32> SectorItem, SideItem; // server: sector / side -> item index
};

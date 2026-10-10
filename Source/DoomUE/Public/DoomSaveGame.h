// Save games through Unreal's SaveGame system (Saved/SaveGames/doom_slotN.sav).
// The header is reflected UPROPERTYs; the world state is a versioned FArchive blob
// written by UDoomWorld::SerializeState (sectors, lines, movers, lights, player,
// monsters, things), so the format stays compact and independent of actor layout.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "DoomSaveGame.generated.h"

UCLASS()
class DOOMUE_API UDoomSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr int32 CurrentVersion = 2; // 2: monster spawn spots for Nightmare respawning

	UPROPERTY() int32 Version = CurrentVersion;
	UPROPERTY() FString IWad;
	UPROPERTY() FString Map;
	UPROPERTY() int32 Skill = 3;
	UPROPERTY() FString Description;
	UPROPERTY() FDateTime Saved;
	UPROPERTY() TArray<uint8> State;
};

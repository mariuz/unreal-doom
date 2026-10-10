// UMAPINFO: per-map names, music, skies, par times, map order, story texts,
// endings and boss actions, from the UMAPINFO lumps of the loaded WADs.
#pragma once

#include "CoreMinimal.h"

struct FDoomBossAction
{
	int32 DoomEdNum = 0;
	int32 Special = 0;
	int32 Tag = 0;
};

struct FDoomMapInfo
{
	FString Map;
	FString LevelName;
	FString Label;          // shown before the name ("E1M1" by default)
	bool bClearLabel = false;
	FString Author;
	FString Music;
	FString Sky;
	FString Next, NextSecret;
	int32 ParTime = -1;
	bool bEndGame = false;  // endgame / endpic / endbunny / endcast
	FString EndPic;
	bool bEndBunny = false;
	bool bEndCast = false;
	bool bNoIntermission = false;
	TArray<FString> InterText, InterTextSecret;
	bool bClearInterText = false;
	FString InterBackdrop;
	FString InterMusic;
	TArray<FDoomBossAction> BossActions;
	bool bClearBossActions = false; // "bossaction = clear": the vanilla boss specials do not apply
};

/** A New Game episode: the map it starts on, its menu graphic and name. */
struct FDoomEpisode
{
	FString Map;
	FString Patch;
	FString Name;
};

class DOOMUE_API FDoomMapInfoSet
{
public:
	/** UMAPINFO "episode" entries in order; bClearEpisodes drops the game's own episodes. */
	TArray<FDoomEpisode> Episodes;
	bool bClearEpisodes = false;

	/** Parses one UMAPINFO lump; later entries for the same map replace earlier ones. */
	int32 Parse(const FString& Text);
	const FDoomMapInfo* Find(const FString& Map) const { return Maps.Find(Map.ToUpper()); }
	int32 Num() const { return Maps.Num(); }
	const TMap<FString, FDoomMapInfo>& All() const { return Maps; }
	void Reset() { Maps.Reset(); Episodes.Reset(); bClearEpisodes = false; }

private:
	TMap<FString, FDoomMapInfo> Maps;
};

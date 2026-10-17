// Baked static-mesh tiles for a map played straight from the WAD (not an imported level), so the
// modern renderer gets distance fields in packaged builds, where meshes cannot be built at runtime.
// -run=DoomContent -baketiles=all writes one set per map under /Game/Doom/Tiles/<WadSet>/<MAP>/.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DoomMapTiles.generated.h"

class UStaticMesh;

UCLASS()
class DOOMUE_API UDoomMapTiles : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "Doom")
	FString MapName;

	/** The WADs the tiles were baked from (WadSetName). */
	UPROPERTY(VisibleAnywhere, Category = "Doom")
	FString WadSet;

	/** ADoomLevel::Signature of the static chunk they draw: the game uses them only when it matches. */
	UPROPERTY(VisibleAnywhere, Category = "Doom")
	int64 Signature = 0;

	UPROPERTY(VisibleAnywhere, Category = "Doom")
	TArray<TSoftObjectPtr<UStaticMesh>> Tiles;

	/** "freedoom1", or "freedoom1+mymod" with PWADs: lower-case file names without extension. */
	static FString WadSetName(const FString& IWad, const TArray<FString>& PWads)
	{
		FString Name = FPaths::GetBaseFilename(IWad).ToLower();
		for (const FString& P : PWads)
		{
			Name += TEXT("+") + FPaths::GetBaseFilename(P).ToLower();
		}
		return Name;
	}

	/** /Game/Doom/Tiles/<WadSet>/<MAP>/ */
	static FString Folder(const FString& WadSet, const FString& MapName)
	{
		return FString::Printf(TEXT("/Game/Doom/Tiles/%s/%s/"), *WadSet, *MapName.ToUpper());
	}

	static FString AssetName(const FString& MapName)
	{
		return FString::Printf(TEXT("DA_%s_Tiles"), *MapName.ToUpper());
	}

	/** The object path of a map's tile set. */
	static FSoftObjectPath AssetPath(const FString& WadSet, const FString& MapName)
	{
		const FString Name = AssetName(MapName);
		return FSoftObjectPath(Folder(WadSet, MapName) + Name + TEXT(".") + Name);
	}
};

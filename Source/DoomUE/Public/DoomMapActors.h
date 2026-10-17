// Doom maps as editable Unreal levels.
//
// Doom > Import Map in the level editor (or -run=DoomContent -importmap=E1M1) saves a map as
// /Game/Doom/Maps/<MAP>.umap holding:
//
//  * ADoomMapActor: names the WAD map and draws its geometry in the editor viewport (the same
//    ADoomLevel builder the game uses, rebuilt whenever something below changes).
//  * One ADoomThingMarker per thing (monsters, items, decorations, player starts), sorted into
//    outliner folders and drawn with the thing's own sprite. Move, rotate, duplicate or delete
//    them, change their type from a list, or set their skill / ambush flags.
//  * ADoomSectorEdit actors, dropped into any room: they override that sector's floor and
//    ceiling heights, light, flats, special and tag ("Read From Sector" fills in the current values).
//  * Baked static mesh tiles of the level's static geometry (/Game/Doom/Maps/<MAP>_Tiles), saved
//    at import or with "Bake Static Meshes": the modern renderer draws them with distance fields
//    for Lumen, also in packaged builds (which cannot bake meshes at runtime).
//
// Pressing Play in such a level starts its map: the game loads the WAD map, applies the sector
// edits and spawns the things from the markers instead of the map's THINGS lump.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DoomLevel.h"
#include "DoomMap.h"
#include "DoomMapActors.generated.h"

class UBillboardComponent;
class UArrowComponent;
class UTextRenderComponent;
class FDoomWad;
class FDoomGraphics;
class UStaticMesh;

UCLASS(HideCategories = (Rendering, Physics, Collision, Input, HLOD, Cooking, Replication, Networking, LOD, Actor, DataLayers, WorldPartition, LevelInstance))
class DOOMUE_API ADoomMapActor : public ADoomLevel
{
	GENERATED_BODY()

public:
	ADoomMapActor();

	/** The WAD map this level edits (E1M1, MAP01...). */
	UPROPERTY(EditAnywhere, Category = "Doom")
	FString MapName = TEXT("E1M1");

	/** Spawn things from this level's Doom Thing markers instead of the map's THINGS lump. */
	UPROPERTY(EditAnywhere, Category = "Doom")
	bool bUseThingMarkers = true;

	/** Draw the map in the editor viewport. */
	UPROPERTY(EditAnywhere, Category = "Doom|Preview")
	bool bShowPreview = true;

	/** Preview with the modern lit materials instead of Doom's palette lighting. */
	UPROPERTY(EditAnywhere, Category = "Doom|Preview")
	bool bPreviewModern = false;

	/** Rebuilds the viewport preview from the WAD, the sector edits and the markers. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void RebuildPreview();

	/** Replaces this level's thing markers with the WAD map's THINGS. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void ImportThings();

	/** Adds a sector edit actor at the middle of the sector under the active viewport's camera. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void AddSectorEdit();

	/** Saves the static geometry, with the current sector edits, as static mesh tile assets. Do it
	    again after editing sectors: the game ignores tiles that no longer match. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void BakeStaticMeshes();

	/** The level's baked tiles and the geometry signature they were baked from. */
	UPROPERTY(VisibleAnywhere, Category = "Doom|Baked")
	TArray<TSoftObjectPtr<UStaticMesh>> BakedTiles;
	UPROPERTY(VisibleAnywhere, Category = "Doom|Baked")
	int64 BakedTileSignature = 0;

	/** Set by the editor module: writes the tile assets (needs editor-only asset tools). */
	static TFunction<bool(ADoomMapActor*)> BakeHook;

	/** Applies the level's sector edits and (when enabled) thing markers to a freshly loaded map. */
	void ApplyEdits(FDoomMap& InMap) const;

	/** The map actor of a world, if it has one. */
	static ADoomMapActor* Find(const UWorld* World);

	/** Editor helper: the WAD map's sector under an Unreal location, or INDEX_NONE. */
	int32 SectorAtLocation(const FVector& Location) const;
	const FDoomMap* GetPreviewMap() const { return PreviewMap.Get(); }

	/** Asks the editor preview to rebuild on the next tick (several edits in a row rebuild once). */
	void RequestPreview();

protected:
	virtual void BeginPlay() override;
#if WITH_EDITOR
	virtual void PostRegisterAllComponents() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void BeginDestroy() override;
#endif

private:
	TUniquePtr<FDoomMap> PreviewMap;
	bool bPreviewQueued = false;
};

UCLASS(HideCategories = (Rendering, Physics, Collision, Input, HLOD, Cooking, Replication, Networking, LOD, Actor, DataLayers, WorldPartition, LevelInstance))
class DOOMUE_API ADoomThingMarker : public AActor
{
	GENERATED_BODY()

public:
	ADoomThingMarker();

	/** The thing to spawn; the list holds every type the port knows. */
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (GetOptions = "GetThingTypeNames"))
	FName ThingType;

	/** Doom's editor number (set from Thing Type; type it directly for PWAD-specific numbers). */
	UPROPERTY(EditAnywhere, Category = "Doom")
	int32 DoomEdNum = 3001;

	UPROPERTY(EditAnywhere, Category = "Doom|Skill")
	bool bEasy = true;   // skills 1 and 2
	UPROPERTY(EditAnywhere, Category = "Doom|Skill")
	bool bMedium = true; // skill 3
	UPROPERTY(EditAnywhere, Category = "Doom|Skill")
	bool bHard = true;   // skills 4 and 5

	/** Deaf: the monster waits until it sees you. */
	UPROPERTY(EditAnywhere, Category = "Doom")
	bool bAmbush = false;

	UPROPERTY(EditAnywhere, Category = "Doom")
	bool bMultiplayerOnly = false;

	/** The map thing this marker stands for (facing from the actor's yaw). */
	FDoomMapThing ToThing() const;
	void FromThing(const FDoomMapThing& Thing);

	UFUNCTION()
	static TArray<FString> GetThingTypeNames();

	/** Display name and outliner folder for a type ("Monsters", "Items"...). */
	static FString TypeName(int32 EdNum);
	static FName FolderFor(int32 EdNum);

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

private:
	void UpdateVisual();

	UPROPERTY(VisibleAnywhere, Category = "Doom")
	TObjectPtr<UArrowComponent> Facing;

	/** Editor-only sprite, created per session (its texture comes from the WAD). */
	UPROPERTY(Transient)
	TObjectPtr<UBillboardComponent> Sprite;
};

UCLASS(HideCategories = (Rendering, Physics, Collision, Input, HLOD, Cooking, Replication, Networking, LOD, Actor, DataLayers, WorldPartition, LevelInstance))
class DOOMUE_API ADoomSectorEdit : public AActor
{
	GENERATED_BODY()

public:
	ADoomSectorEdit();

	/** The sector this actor stands in (follows the actor when moved). */
	UPROPERTY(VisibleAnywhere, Category = "Doom")
	int32 Sector = INDEX_NONE;

	UPROPERTY(EditAnywhere, Category = "Doom", meta = (InlineEditConditionToggle))
	bool bFloor = false;
	/** Floor height in map units. */
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (EditCondition = "bFloor"))
	float FloorHeight = 0;

	UPROPERTY(EditAnywhere, Category = "Doom", meta = (InlineEditConditionToggle))
	bool bCeiling = false;
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (EditCondition = "bCeiling"))
	float CeilingHeight = 128;

	UPROPERTY(EditAnywhere, Category = "Doom", meta = (InlineEditConditionToggle))
	bool bLight = false;
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (EditCondition = "bLight", ClampMin = "0", ClampMax = "255"))
	int32 Light = 160;

	UPROPERTY(EditAnywhere, Category = "Doom", meta = (InlineEditConditionToggle))
	bool bFloorFlat = false;
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (EditCondition = "bFloorFlat", GetOptions = "GetFlatNames"))
	FName FloorFlat;

	UPROPERTY(EditAnywhere, Category = "Doom", meta = (InlineEditConditionToggle))
	bool bCeilingFlat = false;
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (EditCondition = "bCeilingFlat", GetOptions = "GetFlatNames"))
	FName CeilingFlat;

	UPROPERTY(EditAnywhere, Category = "Doom", meta = (InlineEditConditionToggle))
	bool bSpecial = false;
	/** Sector special (1 blink, 7 nukage, 9 secret...). */
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (EditCondition = "bSpecial"))
	int32 Special = 0;

	UPROPERTY(EditAnywhere, Category = "Doom", meta = (InlineEditConditionToggle))
	bool bTag = false;
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (EditCondition = "bTag"))
	int32 Tag = 0;

	/** Copies the WAD's values for this sector into the fields (overrides stay off). */
	UFUNCTION(CallInEditor, Category = "Doom")
	void ReadFromSector();

	void Apply(FDoomMap& Map) const;

	UFUNCTION()
	static TArray<FString> GetFlatNames();

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

private:
	void Refresh(bool bRebuild);

	UPROPERTY(VisibleAnywhere, Category = "Doom")
	TObjectPtr<UTextRenderComponent> Label;
};

/** The WAD as the editor sees it (Project Settings > Doom IWAD and PWADs), loaded on first use. */
namespace DoomEditorData
{
	DOOMUE_API FDoomWad* Wad();
	/** The editor's WADs as baked tile sets are named (UDoomMapTiles::WadSetName). */
	DOOMUE_API FString WadSet();
	DOOMUE_API FDoomGraphics* Graphics();
	DOOMUE_API TArray<FString> MapNames();
}

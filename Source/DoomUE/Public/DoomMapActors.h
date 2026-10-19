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
//  * ADoomLineEdit actors, placed by a line: they override its special, tag, flags and the upper,
//    middle and lower textures of both sides ("Read From Line" fills in the current values).
//  * In the viewport, lines from each trigger to the sectors it works on, and sector outlines
//    coloured by what happens there (doors, lifts, secrets, damaging floors...).
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
#include "DoomMapCheck.h"
#include "DoomMapActors.generated.h"

class UBillboardComponent;
class ULineBatchComponent;
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

	/** The WAD map this level edits (E1M1, MAP01...), or a generated one (RND0002A). */
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

	/** Draw what the specials do: a line from each trigger to the sectors it tags, and outlines of
	 *  door (blue), lift (cyan), teleporter destination (pink), crusher (red), floor and stair
	 *  (orange) and light (yellow) sectors, secrets (magenta) and damaging floors (green). */
	UPROPERTY(EditAnywhere, Category = "Doom|Preview")
	bool bShowConnections = true;

	/** Rebuilds the viewport preview from the WAD, the sector edits and the markers. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void RebuildPreview();

	/** Replaces this level's thing markers with the WAD map's THINGS. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void ImportThings();

	/** Adds a sector edit actor at the middle of the sector under the active viewport's camera. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void AddSectorEdit();

	/** Adds a line edit actor on the line nearest the active viewport's camera. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void AddLineEdit();

	/** Lists the map's problems in the Message Log ("Doom Map Check"): tags with no sectors,
	 *  teleporters with no destination, missing and unknown textures, open sectors, locks without
	 *  their key, things outside the map or too tall for it, no start or no exit... Clicking an entry
	 *  selects its marker or edit actor, or moves the camera there. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void CheckMap();

	/** Writes the map with this level's edits (sector and line edits, thing markers) as a PWAD that
	 *  any Doom port or editor opens: Saved/Maps/<MapName>.wad unless ExportPath says otherwise. */
	UFUNCTION(CallInEditor, Category = "Doom")
	void ExportWad();

	/** Where Export WAD writes (empty: Saved/Maps/<MapName>.wad). */
	UPROPERTY(EditAnywhere, Category = "Doom|Export")
	FString ExportPath;

	/** The map slot in the PWAD (empty: MapName; a generated map goes in as MAP01 or E1M1). */
	UPROPERTY(EditAnywhere, Category = "Doom|Export")
	FString ExportSlot;

	/** The map from the WAD or the generator, with this level's edits applied. */
	bool LoadEditedMap(FDoomMap& Out) const;

	/** The checker on the preview map (edits applied); OutActors[i] is the marker or edit actor
	 *  issue i concerns, if there is one. */
	TArray<FDoomMapIssue> RunCheck(TArray<TWeakObjectPtr<AActor>>& OutActors) const;

	/** Set by the editor module: shows the issues in the Message Log. */
	static TFunction<void(ADoomMapActor*, const TArray<FDoomMapIssue>&, const TArray<TWeakObjectPtr<AActor>>&)> CheckReportHook;

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
	/** Editor helper: the WAD map's line nearest an Unreal location (2D), or INDEX_NONE. */
	int32 LineNearLocation(const FVector& Location) const;
	/** Lines in the connections overlay, and how many of them draw where doors and lifts travel (tests). */
	int32 NumConnectionLines() const;
	int32 NumGhostLines() const { return GhostLines; }
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
	/** The connections overlay (bShowConnections), editor only. */
	void DrawConnections(const FDoomMap& InMap);
	UPROPERTY(Transient)
	TObjectPtr<ULineBatchComponent> Overlay;
	int32 GhostLines = 0;

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

UCLASS(HideCategories = (Rendering, Physics, Collision, Input, HLOD, Cooking, Replication, Networking, LOD, Actor, DataLayers, WorldPartition, LevelInstance))
class DOOMUE_API ADoomLineEdit : public AActor
{
	GENERATED_BODY()

public:
	ADoomLineEdit();

	/** The line this actor edits: the one nearest to it when placed or moved. */
	UPROPERTY(VisibleAnywhere, Category = "Doom")
	int32 Line = INDEX_NONE;

	UPROPERTY(EditAnywhere, Category = "Doom", meta = (InlineEditConditionToggle))
	bool bSpecial = false;
	/** Line special (1 door, 11 exit, 62 lift, 97 teleporter...); 0 for none. */
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (EditCondition = "bSpecial"))
	int32 Special = 0;

	UPROPERTY(EditAnywhere, Category = "Doom", meta = (InlineEditConditionToggle))
	bool bTag = false;
	/** The sectors the special works on (their sector tag). */
	UPROPERTY(EditAnywhere, Category = "Doom", meta = (EditCondition = "bTag"))
	int32 Tag = 0;

	/** Override the flags below (all of them together, as read from the line). */
	UPROPERTY(EditAnywhere, Category = "Doom|Flags")
	bool bFlags = false;
	/** Nothing walks through (two-sided lines; one-sided ones always block). */
	UPROPERTY(EditAnywhere, Category = "Doom|Flags", meta = (EditCondition = "bFlags"))
	bool bImpassable = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Flags", meta = (EditCondition = "bFlags"))
	bool bBlockMonsters = false;
	/** Shown as a one-sided wall on the automap; monsters can't use it. */
	UPROPERTY(EditAnywhere, Category = "Doom|Flags", meta = (EditCondition = "bFlags"))
	bool bSecret = false;
	/** Sound passes only one such line on its way. */
	UPROPERTY(EditAnywhere, Category = "Doom|Flags", meta = (EditCondition = "bFlags"))
	bool bBlockSound = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Flags", meta = (EditCondition = "bFlags"))
	bool bNotOnAutomap = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Flags", meta = (EditCondition = "bFlags"))
	bool bUpperUnpegged = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Flags", meta = (EditCondition = "bFlags"))
	bool bLowerUnpegged = false;

	// textures of the front and back sides ("-" for none)
	UPROPERTY(EditAnywhere, Category = "Doom|Front", meta = (InlineEditConditionToggle))
	bool bFrontUpper = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Front", meta = (EditCondition = "bFrontUpper", GetOptions = "GetTextureNames"))
	FName FrontUpper;
	UPROPERTY(EditAnywhere, Category = "Doom|Front", meta = (InlineEditConditionToggle))
	bool bFrontMiddle = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Front", meta = (EditCondition = "bFrontMiddle", GetOptions = "GetTextureNames"))
	FName FrontMiddle;
	UPROPERTY(EditAnywhere, Category = "Doom|Front", meta = (InlineEditConditionToggle))
	bool bFrontLower = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Front", meta = (EditCondition = "bFrontLower", GetOptions = "GetTextureNames"))
	FName FrontLower;
	UPROPERTY(EditAnywhere, Category = "Doom|Back", meta = (InlineEditConditionToggle))
	bool bBackUpper = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Back", meta = (EditCondition = "bBackUpper", GetOptions = "GetTextureNames"))
	FName BackUpper;
	UPROPERTY(EditAnywhere, Category = "Doom|Back", meta = (InlineEditConditionToggle))
	bool bBackMiddle = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Back", meta = (EditCondition = "bBackMiddle", GetOptions = "GetTextureNames"))
	FName BackMiddle;
	UPROPERTY(EditAnywhere, Category = "Doom|Back", meta = (InlineEditConditionToggle))
	bool bBackLower = false;
	UPROPERTY(EditAnywhere, Category = "Doom|Back", meta = (EditCondition = "bBackLower", GetOptions = "GetTextureNames"))
	FName BackLower;

	/** Copies the WAD's values for this line into the fields (overrides stay off). */
	UFUNCTION(CallInEditor, Category = "Doom")
	void ReadFromLine();

	void Apply(FDoomMap& Map) const;

	UFUNCTION()
	static TArray<FString> GetTextureNames();

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

private:
	/** bPickLine: choose the line nearest the actor again (it was moved). */
	void Refresh(bool bRebuild, bool bPickLine);

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

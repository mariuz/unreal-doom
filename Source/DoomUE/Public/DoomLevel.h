// The level's geometry as Unreal components.
//
//  * One "static" chunk plus one chunk per movable sector (and one per switch line),
//    each a pair of UProceduralMeshComponents: solid (with complex collision, used by
//    the Character Movement Component and by line traces) and non-solid (masked
//    middle textures such as grates).
//  * One section per material (texture) per component, so a chunk is a handful of
//    draw calls; moving a door only updates that door's small chunk.
//  * Sector light is not baked into vertices: every vertex carries its sector index in
//    UV1.x and the material samples a 256-wide light texture (one texel per sector)
//    that the game updates each tic, so flickering lights never touch the meshes.
//  * Modern renderer: the static chunk is also drawn by UStaticMesh tiles (1024 map units
//    square) so Lumen gets mesh distance fields and surface cards; the procedural chunk then
//    stays hidden and only provides collision. Imported levels carry tiles baked in the editor
//    as assets (they cook into packaged builds); otherwise editor builds bake them at load.
//  * Invisible collision for impassable two-sided lines (players and monsters) and
//    monster-blocking lines (a custom collision channel only monsters collide with).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DoomLevel.generated.h"

class FDoomMap;
class FDoomGraphics;
class UProceduralMeshComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;

/** Collision channel used by monsters (DefaultEngine.ini: ECC_GameTraceChannel1 "DoomMonster"). */
#define ECC_DoomMonster ECC_GameTraceChannel1
/** Collision channel used by missiles (ECC_GameTraceChannel2 "DoomMissile"): they ignore impassable lines. */
#define ECC_DoomMissile ECC_GameTraceChannel2

UCLASS(HideDropdown)
class DOOMUE_API ADoomLevel : public AActor
{
	GENERATED_BODY()

public:
	ADoomLevel();

	/** Builds everything at once: the map's pictures and chunks are still made in parallel. */
	void Build(FDoomMap& InMap, FDoomGraphics& InGfx, const TSet<int32>& MovableSectors, bool bModern, FName SkyTexture);

	/** Generated geometry of one chunk: solid and masked sections. */
	struct FChunkSections;
	/**
	 * Build in steps, for loading behind a loading screen:
	 *  * BeginBuild (game thread): the chunks and their components.
	 *  * GenerateChunks (any thread, once the map's pictures are cached: FDoomGraphics::CachePrefetch):
	 *    every chunk's geometry, in parallel. It only reads the map and the picture caches.
	 *  * CommitChunks (game thread): mesh sections, collision and the modern renderer's tiles, chunk
	 *    by chunk until Deadline (FPlatformTime::Seconds); true when all are done.
	 *  * FinishBuild (game thread).
	 */
	void BeginBuild(FDoomMap& InMap, FDoomGraphics& InGfx, const TSet<int32>& MovableSectors, bool bModern, FName SkyTexture);
	void GenerateChunks(TArray<FChunkSections>& Out) const;
	bool CommitChunks(TArray<FChunkSections>& Sections, int32& Next, double Deadline);
	void FinishBuild();
	int32 NumChunks() const { return Chunks.Num(); }

	/** Destroys all generated geometry (the editor preview rebuilds through this). */
	void ClearGeometry();

	/** Editor preview: collision cooks in the background and nothing is baked. */
	bool bEditorPreview = false;

	/** A sector's floor/ceiling or a side's texture changed: rebuild the affected chunks on Flush. */
	void SectorChanged(int32 Sector);
	void LineChanged(int32 Line);
	void Flush();

	/** Upload the sector light levels (call when any changed). */
	void UpdateSectorLights();
	/** Animated flats and walls (every tic; changes textures every 8 tics). */
	void AnimateTextures(int32 Tic);

	UMaterialInstanceDynamic* GetSurfaceMaterial(FName Texture, bool bFlat);

	/** Material instances that need the global light texture and other per-level params. */
	UTexture2D* GetSectorLightTexture() const { return SectorLightTex; }

	/** Static mesh tiles drawing the static chunk (0 when not baked). */
	int32 NumBakedTiles() const { return BakedComponents.Num(); }
	/** True when the tiles came from saved assets (an imported level's bake), not a runtime bake. */
	bool UsesSavedTiles() const { return bSavedTiles; }

	/** Generated geometry of one material section. */
	struct FSectionData
	{
		TArray<FVector> V;
		TArray<int32> I;
		TArray<FVector> N;
		TArray<FVector2D> UV0, UV1;
		TArray<FLinearColor> C;
	};
	struct FChunkSections
	{
		TMap<FName, FSectionData> Solid, NonSolid;
	};
	/** One static mesh tile: its grid cell and, per material slot, the section key it draws. */
	struct FTile
	{
		FIntPoint Cell;
		TArray<FName> Slots;
		TMap<FName, FSectionData> Sections;
	};
	/** The static chunk's sections as they are now (the editor bakes them into tile assets). */
	void GetStaticSections(TMap<FName, FSectionData>& Solid, TMap<FName, FSectionData>& NonSolid);
	/** All chunks' sections merged (static, movers and switch lines): map exporters. */
	void GetAllSections(TMap<FName, FSectionData>& Solid, TMap<FName, FSectionData>& NonSolid);
	/** A checksum of a chunk's geometry: saved tiles are used only when it matches. */
	static uint32 Signature(const TMap<FName, FSectionData>& Solid, const TMap<FName, FSectionData>& NonSolid);
	/** Splits sections into 1024-unit tiles (by triangle centroid), sharing vertices per tile. */
	static void SplitTiles(const TMap<FName, FSectionData>& Solid, const TMap<FName, FSectionData>& NonSolid, TArray<FTile>& Out);
#if WITH_EDITOR
	/** Fills a static mesh from a tile: mesh description, slots named by section key, build settings
	    for distance fields. Material(Key) gives each slot's material. Call Build afterwards. */
	static void FillTileMesh(UStaticMesh* Mesh, const FTile& Tile, TFunctionRef<UMaterialInterface*(FName)> Material);
#endif
	/** Procedural mesh components currently drawing or colliding (tests). */
	int32 NumMeshComponents() const { return MeshComponents.Num(); }

private:
	struct FChunk
	{
		TObjectPtr<UProceduralMeshComponent> Solid;
		TObjectPtr<UProceduralMeshComponent> NonSolid;
		TArray<int32> Sectors;   // floors and ceilings drawn here
		TArray<int32> Lines;     // walls drawn here
		TMap<FName, int32> SolidSections, NonSolidSections;
		TArray<int32> SolidCounts, NonSolidCounts; // vertex counts per section
		bool bDynamic = false;
		bool bDirty = true;
	};

	void Generate(const FChunk& Chunk, TMap<FName, FSectionData>& OutSolid, TMap<FName, FSectionData>& OutNonSolid) const;
	void EmitPlanes(int32 Sector, TMap<FName, FSectionData>& Out) const;
	void EmitLine(int32 Line, bool bDynamic, TMap<FName, FSectionData>& OutSolid, TMap<FName, FSectionData>& OutNonSolid) const;
	void EmitQuad(FSectionData& S, const FVector2f& A, const FVector2f& B, float Bottom, float Top,
		float U0, float U1, float TexTop, float TexW, float TexH, int32 Sector, float Contrast, float Kind) const;
	/** Commits one chunk's generated sections (and bakes the static chunk's tiles in the modern renderer). */
	void CommitChunk(int32 Index, FChunkSections& Sections);
	void Commit(FChunk& Chunk, UProceduralMeshComponent* Comp, TMap<FName, int32>& Sections, TArray<int32>& Counts, TMap<FName, FSectionData>& Data, bool bCollision);
	void BuildBlockingCollision();
	FName SectionKey(FName Texture, bool bFlat, bool bSky) const;
	UMaterialInterface* MaterialForKey(FName Key);
	UProceduralMeshComponent* NewMeshComponent(const FString& Name, bool bCollision);
	/** Bakes the static chunk's sections into static mesh tiles (modern renderer, editor builds). */
	void BakeStaticChunk(const TMap<FName, FSectionData>& Solid, const TMap<FName, FSectionData>& NonSolid);
	/** The saved tiles of this map's imported level, when they match the static chunk. */
	bool FindSavedTiles(uint32 Sig, TArray<UStaticMesh*>& Out) const;
	/** Draws the static chunk with these meshes (slots get this level's material instances). */
	void ShowTiles(const TArray<UStaticMesh*>& Meshes, bool bSaved);

	FDoomMap* Map = nullptr;
	FDoomGraphics* Gfx = nullptr;
	bool bModern = false;

	TArray<FChunk> Chunks;
	TArray<TArray<int32>> SectorChunks; // sector -> chunks depending on it
	TArray<int32> LineChunk;            // line -> chunk

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> MeshComponents;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> BakedComponents;
	uint32 BakedSignature = 0;
	bool bSavedTiles = false;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WorldMaterial;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SkyMaterial;
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> SectorLightTex;

	TArray<FColor> SectorLightPixels;
	int32 SectorLightRows = 1;
	FName SkyKey;
};

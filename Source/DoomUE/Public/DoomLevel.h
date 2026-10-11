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
//  * Modern renderer: the static chunk is also baked into runtime UStaticMesh tiles
//    (1024 map units square) so Lumen gets mesh distance fields and surface cards;
//    the procedural chunk then stays hidden and only provides collision.
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
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;

/** Collision channel used by monsters (DefaultEngine.ini: ECC_GameTraceChannel1 "DoomMonster"). */
#define ECC_DoomMonster ECC_GameTraceChannel1
/** Collision channel used by missiles (ECC_GameTraceChannel2 "DoomMissile"): they ignore impassable lines. */
#define ECC_DoomMissile ECC_GameTraceChannel2

UCLASS()
class DOOMUE_API ADoomLevel : public AActor
{
	GENERATED_BODY()

public:
	ADoomLevel();

	void Build(FDoomMap& InMap, FDoomGraphics& InGfx, const TSet<int32>& MovableSectors, bool bModern, FName SkyTexture);

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

private:
	struct FSectionData
	{
		TArray<FVector> V;
		TArray<int32> I;
		TArray<FVector> N;
		TArray<FVector2D> UV0, UV1;
		TArray<FLinearColor> C;
	};
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

	void Generate(FChunk& Chunk, TMap<FName, FSectionData>& OutSolid, TMap<FName, FSectionData>& OutNonSolid);
	void EmitPlanes(int32 Sector, TMap<FName, FSectionData>& Out);
	void EmitLine(int32 Line, bool bDynamic, TMap<FName, FSectionData>& OutSolid, TMap<FName, FSectionData>& OutNonSolid);
	void EmitQuad(FSectionData& S, const FVector2f& A, const FVector2f& B, float Bottom, float Top,
		float U0, float U1, float TexTop, float TexW, float TexH, int32 Sector, float Contrast, float Kind);
	void Commit(FChunk& Chunk, UProceduralMeshComponent* Comp, TMap<FName, int32>& Sections, TArray<int32>& Counts, TMap<FName, FSectionData>& Data, bool bCollision);
	void BuildBlockingCollision();
	FName SectionKey(FName Texture, bool bFlat, bool bSky) const;
	UMaterialInterface* MaterialForKey(FName Key);
	UProceduralMeshComponent* NewMeshComponent(const FString& Name, bool bCollision);
	/** Bakes the static chunk's sections into static mesh tiles (modern renderer, editor builds). */
	void BakeStaticChunk(const TMap<FName, FSectionData>& Solid, const TMap<FName, FSectionData>& NonSolid);
	static uint32 Signature(const TMap<FName, FSectionData>& Solid, const TMap<FName, FSectionData>& NonSolid);

	FDoomMap* Map = nullptr;
	FDoomGraphics* Gfx = nullptr;
	bool bModern = false;

	TArray<FChunk> Chunks;
	TArray<TArray<int32>> SectorChunks; // sector -> chunks depending on it
	TArray<int32> LineChunk;            // line -> chunk

	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> MeshComponents;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> BakedComponents;
	uint32 BakedSignature = 0;

	UPROPERTY()
	TMap<FName, TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> WorldMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SkyMaterial;
	UPROPERTY()
	TObjectPtr<UTexture2D> SectorLightTex;

	TArray<FColor> SectorLightPixels;
	int32 SectorLightRows = 1;
	FName SkyKey;
};

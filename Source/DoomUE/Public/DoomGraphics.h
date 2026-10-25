// Doom pictures: column patches, composite wall textures (TEXTURE1/2 + PNAMES),
// flats and sprites, decoded through PLAYPAL into transient UTexture2Ds.
#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"

class FDoomWad;
class UTexture2D;
class FDoomMap;

struct FDoomPicture
{
	FName Name;
	int32 Width = 0, Height = 0;
	int32 LeftOffset = 0, TopOffset = 0;
	/** Palette index per pixel, -1 = transparent. Row-major from the top. */
	TArray<int16> Indices;
	bool bMasked = false;
	UTexture2D* Texture = nullptr;
	/** Palette indices (R = index, A = opaque) for palette-exact COLORMAP lighting. */
	UTexture2D* IndexTexture = nullptr;

	bool IsValid() const { return Width > 0 && Height > 0; }
};

/** Pixels of a picture's two textures (colour, palette index) with their mip chains; made off the game thread. */
struct FDoomPictureMips
{
	TArray<TArray<FColor>> Color, Index;
};

/** The wall textures and flats a map draws, decoded on worker threads before its level is built. */
struct FDoomPicturePrefetch
{
	struct FItem
	{
		FName Name;
		bool bFlat = false;
		TSharedPtr<FDoomPicture> Pic; // null when the WAD lacks it
		FDoomPictureMips Mips;
	};
	TArray<FItem> Items;
	int32 NextToCreate = 0;
};

struct FDoomSpriteFrame
{
	const FDoomPicture* Picture = nullptr;
	bool bMirror = false;
};

class DOOMUE_API FDoomGraphics : public FGCObject
{
public:
	/** For the map generator (DoomMapGen::Generate): whether these graphics have a wall texture or flat
	 *  of that name. Null graphics: no check. */
	static TFunction<bool(FName, bool)> PictureCheck(const FDoomGraphics* Gfx);
	explicit FDoomGraphics(const FDoomWad& InWad);
	virtual ~FDoomGraphics() override;

	/** Wall texture by name (composed from patches). Null when missing. */
	const FDoomPicture* Texture(FName Name);
	/** Floor/ceiling flat by name. */
	const FDoomPicture* Flat(FName Name);
	/** Every flat name in lump order (editor lists). */
	const TArray<FName>& FlatNames() const { return FlatOrder; }
	/** The WAD's wall textures, in TEXTURE1/2 order. */
	const TArray<FName>& TextureNames() const { return TexOrder; }
	/** A patch lump (HUD graphics, menu, etc). */
	const FDoomPicture* Patch(const FString& LumpName);
	/** Sprite frame: prefix "TROO", frame 'A', rotation 1..8 (0 = any). */
	/** Translation 1-3 recolours the marine's green (palette 0x70-0x7F) grey, brown or red: Doom's
	 *  player colours for players 2-4 (R_InitTranslationTables). */
	FDoomSpriteFrame SpriteFrame(const FString& Prefix, TCHAR Frame, int32 Rotation, int32 Translation = 0);
	bool SpriteHasRotations(const FString& Prefix, TCHAR Frame) const;

	/** Texture height in pixels (for shortest-lower-texture specials); 0 if missing. */
	int32 TextureHeight(FName Name);

	/** Current frame of an animated wall or flat at the given tic. */
	FName AnimatedFrame(FName Name, bool bFlat, int32 Tic) const;
	bool IsAnimated(FName Name, bool bFlat) const;

	/** SW1xxx <-> SW2xxx partner, or NAME_None. */
	FName SwitchPartner(FName Name) const;
	/** A wall texture defined in TEXTURE1/2 (the map checker's unknown names). */
	bool HasTexture(FName Name) const { return TexDefs.Contains(Name); }

	/**
	 * Loading a map's pictures off the game thread, in three steps:
	 *  1. CollectPrefetch (game thread): the map's uncached wall textures and flats, with their
	 *     animation frames and switch partners.
	 *  2. DecodePrefetch (any thread): composes them from patches and builds the mip chains, in
	 *     parallel; it reads only the WAD and the texture tables.
	 *  3. CachePrefetch, then CreatePrefetchTextures (game thread): the pictures enter the caches (so
	 *     lookups while the level geometry is generated on workers are read-only), and their GPU
	 *     textures are created, in slices up to a deadline (FPlatformTime::Seconds) so a loading
	 *     screen keeps drawing. Returns true when all are created.
	 */
	TSharedRef<FDoomPicturePrefetch> CollectPrefetch(const FDoomMap& Map) const;
	void DecodePrefetch(FDoomPicturePrefetch& Prefetch) const;
	void CachePrefetch(FDoomPicturePrefetch& Prefetch);
	bool CreatePrefetchTextures(FDoomPicturePrefetch& Prefetch, double Deadline);

	/** Sky texture name for a map. */
	static FName SkyTextureFor(const FString& MapName);
	static bool IsSkyFlat(FName Name);

	/** Palette lookup textures for the classic shader path (palette and COLORMAP as images). */
	UTexture2D* ColormapTexture();

	/** Applies colormap row Row to a picture, returning a new transient texture (weapon sprites under invulnerability etc.). */
	UTexture2D* MakeTextureFromIndices(const FDoomPicture& Pic, int32 ColormapRow, int32 PaletteIndex);

	const FDoomWad& GetWad() const { return Wad; }

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FDoomGraphics"); }

private:
	struct FTexDef
	{
		FName Name;
		int32 Width = 0, Height = 0;
		struct FPatchRef { int32 X, Y, PNameIndex; };
		TArray<FPatchRef> Patches;
	};
	struct FAnimGroup
	{
		TArray<FName> Frames;
		bool bFlat = false;
		int32 Speed = 8;
	};

	void ParseTextureDefs();
	void IndexFlatsAndSprites();
	void BuildAnimations();
	bool DecodePatch(TConstArrayView<uint8> Data, FDoomPicture& Out) const;
	bool ComposeTexture(FName Name, FDoomPicture& Out) const;
	bool ComposeFlat(FName Name, FDoomPicture& Out) const;
	/** CPU side of a picture's textures (thread-safe), then the GPU side (game thread). */
	void PrepareMips(const FDoomPicture& Pic, bool bMips, FDoomPictureMips& Out) const;
	void CreateTextures(FDoomPicture& Pic, const FDoomPictureMips& Mips, bool bWrap);
	UTexture2D* CreateTexture(int32 Width, int32 Height, const TArray<TArray<FColor>>& Mips, bool bSRGB, bool bWrap, FName Name);
	void Upload(FDoomPicture& Pic, bool bWrap, bool bMips);

public:
	/** A picture through one COLORMAP row and PLAYPAL palette (weapon light, screen palettes), cached. */
	UTexture2D* ColormappedPicture(const FDoomPicture& Pic, int32 Row, int32 Palette = 0);
	/** Rows in ColormapTexture: 34 per PLAYPAL palette. */
	int32 ColormapRows() const;
private:
	TMap<TPair<const FDoomPicture*, int32>, TObjectPtr<UTexture2D>> ColormappedCache;

	const FDoomWad& Wad;
	TArray<int32> PNames;   // lump index per PNAMES entry
	TMap<FName, FTexDef> TexDefs;
	TArray<FName> TexOrder;
	TMap<FName, int32> FlatLumps;
	TArray<FName> FlatOrder;
	struct FSpriteLump { int32 Lump; bool bMirror; };
	TMap<FString, FSpriteLump> SpriteLumps; // "TROOA1" -> lump
	TMap<FName, TSharedPtr<FDoomPicture>> TextureCache, FlatCache, PatchCache;
	TMap<int32, TSharedPtr<FDoomPicture>> SpriteCache;
	TMap<int64, TSharedPtr<FDoomPicture>> TranslatedSprites; // (lump << 2) | translation
	TArray<FAnimGroup> Anims;
	TMap<FName, FName> SwitchPairs;
	bool bSwitchLump = false;
	TMap<FName, TPair<int32, int32>> AnimLookupWall, AnimLookupFlat; // name -> (group, index)
	TArray<TObjectPtr<UTexture2D>> Owned;
	TObjectPtr<UTexture2D> Colormap = nullptr;
};

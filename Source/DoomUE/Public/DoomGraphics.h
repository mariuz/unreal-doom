// Doom pictures: column patches, composite wall textures (TEXTURE1/2 + PNAMES),
// flats and sprites, decoded through PLAYPAL into transient UTexture2Ds.
#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"

class FDoomWad;
class UTexture2D;

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

struct FDoomSpriteFrame
{
	const FDoomPicture* Picture = nullptr;
	bool bMirror = false;
};

class DOOMUE_API FDoomGraphics : public FGCObject
{
public:
	explicit FDoomGraphics(const FDoomWad& InWad);
	virtual ~FDoomGraphics() override;

	/** Wall texture by name (composed from patches). Null when missing. */
	const FDoomPicture* Texture(FName Name);
	/** Floor/ceiling flat by name. */
	const FDoomPicture* Flat(FName Name);
	/** A patch lump (HUD graphics, menu, etc). */
	const FDoomPicture* Patch(const FString& LumpName);
	/** Sprite frame: prefix "TROO", frame 'A', rotation 1..8 (0 = any). */
	FDoomSpriteFrame SpriteFrame(const FString& Prefix, TCHAR Frame, int32 Rotation);
	bool SpriteHasRotations(const FString& Prefix, TCHAR Frame) const;

	/** Texture height in pixels (for shortest-lower-texture specials); 0 if missing. */
	int32 TextureHeight(FName Name);

	/** Current frame of an animated wall or flat at the given tic. */
	FName AnimatedFrame(FName Name, bool bFlat, int32 Tic) const;
	bool IsAnimated(FName Name, bool bFlat) const;

	/** SW1xxx <-> SW2xxx partner, or NAME_None. */
	FName SwitchPartner(FName Name);

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
	void Upload(FDoomPicture& Pic, bool bWrap, bool bMips);
	void UploadIndices(FDoomPicture& Pic, bool bWrap, bool bMips);
	int32 NearestPaletteIndex(int32 R, int32 G, int32 B);
	TMap<uint32, uint8> NearestCache;

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
	TArray<FAnimGroup> Anims;
	TMap<FName, FName> SwitchPairs;
	bool bSwitchLump = false;
	TMap<FName, TPair<int32, int32>> AnimLookupWall, AnimLookupFlat; // name -> (group, index)
	TArray<TObjectPtr<UTexture2D>> Owned;
	TObjectPtr<UTexture2D> Colormap = nullptr;
};

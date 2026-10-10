// WAD archives: the IWAD plus any PWADs stacked on top (Doom's -file).
#pragma once

#include "CoreMinimal.h"

struct FWadLump
{
	FName Name;          // upper-case, up to 8 chars
	FString NameStr;
	int32 FileIndex = 0;
	int32 Offset = 0;
	int32 Size = 0;
};

class DOOMUE_API FDoomWad
{
public:
	/** Adds a WAD file; later files override earlier ones. */
	bool AddFile(const FString& Path);

	int32 NumLumps() const { return Lumps.Num(); }
	const FWadLump& Lump(int32 Index) const { return Lumps[Index]; }

	/** Last lump with this name (PWADs win), or INDEX_NONE. */
	int32 FindLump(const FString& Name) const;
	/** Lump index of Name searching forward from Start (used for map lumps after a marker). */
	int32 FindLumpAfter(const FString& Name, int32 Start, int32 Limit) const;

	TConstArrayView<uint8> LumpData(int32 Index) const;
	TConstArrayView<uint8> LumpData(const FString& Name) const;

	/** 256 RGB colours of PLAYPAL palette N (0 = normal, 1-8 red, 9-12 gold, 13 green). */
	const TArray<FColor>& Palette(int32 N = 0) const { return Palettes.IsValidIndex(N) ? Palettes[N] : Palettes[0]; }
	int32 NumPalettes() const { return Palettes.Num(); }
	/** 34 rows of 256 palette indices. */
	const TArray<uint8>& Colormap() const { return ColormapData; }

	/** Map marker names in WAD order (E1M1.. or MAP01..). */
	const TArray<FString>& MapNames() const { return Maps; }
	bool IsDoom2() const { return bDoom2; }
	bool IsValid() const { return Lumps.Num() > 0 && Palettes.Num() > 0; }

	/** Called after all files are added. */
	void Finalize();

private:
	TArray<TArray<uint8>> Files;
	TArray<FWadLump> Lumps;
	TMap<FName, int32> LastByName;
	TArray<TArray<FColor>> Palettes;
	TArray<uint8> ColormapData;
	TArray<FString> Maps;
	bool bDoom2 = false;
};

/** Little-endian readers over WAD bytes. */
namespace WadRead
{
	inline int16 S16(const uint8* P) { return (int16)(P[0] | (P[1] << 8)); }
	inline uint16 U16(const uint8* P) { return (uint16)(P[0] | (P[1] << 8)); }
	inline int32 S32(const uint8* P) { return (int32)(P[0] | (P[1] << 8) | (P[2] << 16) | ((uint32)P[3] << 24)); }
	FString Name8(const uint8* P);
}

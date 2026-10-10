#include "DoomWad.h"
#include "DoomUE.h"
#include "Misc/FileHelper.h"

FString WadRead::Name8(const uint8* P)
{
	ANSICHAR Buf[9];
	int32 N = 0;
	for (; N < 8 && P[N]; ++N)
	{
		Buf[N] = FChar::ToUpper((ANSICHAR)P[N]);
	}
	Buf[N] = 0;
	return FString(ANSI_TO_TCHAR(Buf));
}

bool FDoomWad::AddFile(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogDoom, Error, TEXT("Cannot read WAD %s"), *Path);
		return false;
	}
	if (Bytes.Num() < 12 || (FMemory::Memcmp(Bytes.GetData(), "IWAD", 4) != 0 && FMemory::Memcmp(Bytes.GetData(), "PWAD", 4) != 0))
	{
		UE_LOG(LogDoom, Error, TEXT("%s is not a WAD"), *Path);
		return false;
	}
	const int32 Count = WadRead::S32(&Bytes[4]);
	const int32 DirOfs = WadRead::S32(&Bytes[8]);
	if (Count < 0 || DirOfs < 0 || (int64)DirOfs + (int64)Count * 16 > Bytes.Num())
	{
		UE_LOG(LogDoom, Error, TEXT("%s has a broken directory"), *Path);
		return false;
	}
	const int32 FileIndex = Files.Num();
	for (int32 i = 0; i < Count; ++i)
	{
		const uint8* E = &Bytes[DirOfs + i * 16];
		FWadLump L;
		L.Offset = WadRead::S32(E);
		L.Size = WadRead::S32(E + 4);
		L.NameStr = WadRead::Name8(E + 8);
		L.Name = FName(*L.NameStr);
		L.FileIndex = FileIndex;
		if (L.Offset < 0 || L.Size < 0 || (int64)L.Offset + L.Size > Bytes.Num())
		{
			L.Size = 0;
		}
		Lumps.Add(L);
	}
	Files.Add(MoveTemp(Bytes));
	UE_LOG(LogDoom, Log, TEXT("Added %s: %d lumps"), *Path, Count);
	return true;
}

void FDoomWad::Finalize()
{
	LastByName.Reset();
	Maps.Reset();
	for (int32 i = 0; i < Lumps.Num(); ++i)
	{
		LastByName.Add(Lumps[i].Name, i);
		const FString& N = Lumps[i].NameStr;
		const bool bExMy = N.Len() == 4 && N[0] == 'E' && FChar::IsDigit(N[1]) && N[2] == 'M' && FChar::IsDigit(N[3]);
		const bool bMapXX = N.Len() == 5 && N.StartsWith(TEXT("MAP")) && FChar::IsDigit(N[3]) && FChar::IsDigit(N[4]);
		if ((bExMy || bMapXX) && i + 1 < Lumps.Num() && Lumps[i + 1].NameStr == TEXT("THINGS"))
		{
			Maps.AddUnique(N);
			bDoom2 |= bMapXX;
		}
	}
	Maps.Sort();

	Palettes.Reset();
	TConstArrayView<uint8> Pal = LumpData(TEXT("PLAYPAL"));
	for (int32 p = 0; (p + 1) * 768 <= Pal.Num(); ++p)
	{
		TArray<FColor>& Out = Palettes.AddDefaulted_GetRef();
		Out.SetNum(256);
		for (int32 c = 0; c < 256; ++c)
		{
			Out[c] = FColor(Pal[p * 768 + c * 3], Pal[p * 768 + c * 3 + 1], Pal[p * 768 + c * 3 + 2], 255);
		}
	}
	TConstArrayView<uint8> Cm = LumpData(TEXT("COLORMAP"));
	ColormapData = TArray<uint8>(Cm.GetData(), Cm.Num());
	if (ColormapData.Num() < 34 * 256)
	{
		// Synthesize: 32 darkening levels and an inverted grey row.
		ColormapData.SetNumZeroed(34 * 256);
	}
}

int32 FDoomWad::FindLump(const FString& Name) const
{
	const int32* Found = LastByName.Find(FName(*Name.ToUpper()));
	return Found ? *Found : INDEX_NONE;
}

int32 FDoomWad::FindLumpAfter(const FString& Name, int32 Start, int32 Limit) const
{
	for (int32 i = Start; i < FMath::Min(Limit, Lumps.Num()); ++i)
	{
		if (Lumps[i].NameStr == Name)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

TConstArrayView<uint8> FDoomWad::LumpData(int32 Index) const
{
	if (!Lumps.IsValidIndex(Index))
	{
		return {};
	}
	const FWadLump& L = Lumps[Index];
	return TConstArrayView<uint8>(Files[L.FileIndex].GetData() + L.Offset, L.Size);
}

TConstArrayView<uint8> FDoomWad::LumpData(const FString& Name) const
{
	return LumpData(FindLump(Name));
}

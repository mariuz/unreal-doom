#include "DoomGraphics.h"
#include "DoomWad.h"
#include "DoomUE.h"
#include "DoomInfo.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "DoomMap.h"
#include "Async/ParallelFor.h"

using namespace WadRead;

namespace
{
	// Animated textures and flats: first and last frame names; the frames are
	// everything between them in TEXTURE1/2 or flat order (the vanilla rule).
	struct FAnimDef { bool bFlat; const TCHAR* First; const TCHAR* Last; };
	const FAnimDef GAnimDefs[] =
	{
		{ true,  TEXT("NUKAGE1"),  TEXT("NUKAGE3") },
		{ true,  TEXT("FWATER1"),  TEXT("FWATER4") },
		{ true,  TEXT("SWATER1"),  TEXT("SWATER4") },
		{ true,  TEXT("LAVA1"),    TEXT("LAVA4") },
		{ true,  TEXT("BLOOD1"),   TEXT("BLOOD3") },
		{ true,  TEXT("RROCK05"),  TEXT("RROCK08") },
		{ true,  TEXT("SLIME01"),  TEXT("SLIME04") },
		{ true,  TEXT("SLIME05"),  TEXT("SLIME08") },
		{ true,  TEXT("SLIME09"),  TEXT("SLIME12") },
		{ false, TEXT("BLODGR1"),  TEXT("BLODGR4") },
		{ false, TEXT("SLADRIP1"), TEXT("SLADRIP3") },
		{ false, TEXT("BLODRIP1"), TEXT("BLODRIP4") },
		{ false, TEXT("FIREWALA"), TEXT("FIREWALL") },
		{ false, TEXT("GSTFONT1"), TEXT("GSTFONT3") },
		{ false, TEXT("FIRELAV3"), TEXT("FIRELAVA") },
		{ false, TEXT("FIREMAG1"), TEXT("FIREMAG3") },
		{ false, TEXT("FIREBLU1"), TEXT("FIREBLU2") },
		{ false, TEXT("ROCKRED1"), TEXT("ROCKRED3") },
		{ false, TEXT("BFALL1"),   TEXT("BFALL4") },
		{ false, TEXT("SFALL1"),   TEXT("SFALL4") },
		{ false, TEXT("WFALL1"),   TEXT("WFALL4") },
		{ false, TEXT("DBRAIN1"),  TEXT("DBRAIN4") },
	};

	bool IsPow2(int32 V) { return V > 0 && (V & (V - 1)) == 0; }

	/**
	 * A transient texture holding Pixels as mip 0, with no GPU resource yet. CreateTransient with
	 * pixel data starts creating the resource on the render thread at once, which races with
	 * adding mips or changing properties afterwards (an intermittent D3D12 assertion); callers
	 * add mips, set filtering and addressing, then call UpdateResource exactly once.
	 */
	UTexture2D* NewTexture(int32 W, int32 H, const TArray<FColor>& Pixels, FName Name)
	{
		UTexture2D* Tex = UTexture2D::CreateTransient(W, H, PF_B8G8R8A8, Name);
		if (Tex && Pixels.Num() == W * H)
		{
			FTexture2DMipMap& Mip0 = Tex->GetPlatformData()->Mips[0];
			void* Dest = Mip0.BulkData.Lock(LOCK_READ_WRITE);
			FMemory::Memcpy(Dest, Pixels.GetData(), (SIZE_T)W * H * 4);
			Mip0.BulkData.Unlock();
		}
		return Tex;
	}
}

FDoomGraphics::FDoomGraphics(const FDoomWad& InWad)
	: Wad(InWad)
{
	ParseTextureDefs();
	IndexFlatsAndSprites();
	BuildAnimations();
}

FDoomGraphics::~FDoomGraphics() = default;

void FDoomGraphics::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObjects(Owned);
	Collector.AddReferencedObject(Colormap);
	for (auto& Pair : ColormappedCache)
	{
		Collector.AddReferencedObject(Pair.Value);
	}
}

void FDoomGraphics::ParseTextureDefs()
{
	TConstArrayView<uint8> PN = Wad.LumpData(TEXT("PNAMES"));
	if (PN.Num() >= 4)
	{
		const int32 Count = FMath::Min(S32(PN.GetData()), (PN.Num() - 4) / 8);
		PNames.SetNum(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			PNames[i] = Wad.FindLump(Name8(&PN[4 + i * 8]));
		}
	}
	for (const TCHAR* LumpName : { TEXT("TEXTURE1"), TEXT("TEXTURE2") })
	{
		TConstArrayView<uint8> T = Wad.LumpData(LumpName);
		if (T.Num() < 4)
		{
			continue;
		}
		const int32 Count = S32(T.GetData());
		for (int32 i = 0; i < Count && 4 + i * 4 + 4 <= T.Num(); ++i)
		{
			const int32 Ofs = S32(&T[4 + i * 4]);
			if (Ofs < 0 || Ofs + 22 > T.Num())
			{
				continue;
			}
			const uint8* P = &T[Ofs];
			FTexDef Def;
			Def.Name = FName(*Name8(P));
			Def.Width = S16(P + 12);
			Def.Height = S16(P + 14);
			const int32 NumPatches = S16(P + 20);
			for (int32 k = 0; k < NumPatches && Ofs + 22 + k * 10 + 10 <= T.Num(); ++k)
			{
				const uint8* Q = P + 22 + k * 10;
				Def.Patches.Add({ S16(Q), S16(Q + 2), S16(Q + 4) });
			}
			if (!TexDefs.Contains(Def.Name))
			{
				TexOrder.Add(Def.Name);
			}
			TexDefs.Add(Def.Name, MoveTemp(Def));
		}
	}
	UE_LOG(LogDoom, Log, TEXT("%d wall textures, %d patch names"), TexDefs.Num(), PNames.Num());
}

void FDoomGraphics::IndexFlatsAndSprites()
{
	bool bInFlats = false, bInSprites = false;
	for (int32 i = 0; i < Wad.NumLumps(); ++i)
	{
		const FString& N = Wad.Lump(i).NameStr;
		if (N == TEXT("F_START") || N == TEXT("FF_START")) { bInFlats = true; continue; }
		if (N == TEXT("F_END") || N == TEXT("FF_END")) { bInFlats = false; continue; }
		if (N == TEXT("S_START") || N == TEXT("SS_START")) { bInSprites = true; continue; }
		if (N == TEXT("S_END") || N == TEXT("SS_END")) { bInSprites = false; continue; }
		if (bInFlats && Wad.Lump(i).Size == 4096)
		{
			const FName FN(*N);
			if (!FlatLumps.Contains(FN))
			{
				FlatOrder.Add(FN);
			}
			FlatLumps.Add(FN, i);
		}
		else if (bInSprites && N.Len() >= 6)
		{
			const FString Prefix = N.Left(4);
			SpriteLumps.Add(Prefix + N.Mid(4, 2), { i, false });
			if (N.Len() >= 8)
			{
				SpriteLumps.Add(Prefix + N.Mid(6, 2), { i, true });
			}
		}
	}
	UE_LOG(LogDoom, Log, TEXT("%d flats, %d sprite frames"), FlatLumps.Num(), SpriteLumps.Num());
}

void FDoomGraphics::BuildAnimations()
{
	// Boom's ANIMATED lump (23-byte records: type, last name, first name, speed) replaces the
	// built-in table; type bit 0 = wall texture, 0xFF ends the list.
	struct FDef { bool bFlat; FString First, Last; int32 Speed; };
	TArray<FDef> Defs;
	TConstArrayView<uint8> Animated = Wad.LumpData(TEXT("ANIMATED"));
	if (Animated.Num() > 0)
	{
		for (int32 Pos = 0; Pos + 23 <= Animated.Num() && Animated[Pos] != 0xFF; Pos += 23)
		{
			const uint8* R = &Animated[Pos];
			Defs.Add({ (R[0] & 1) == 0, Name8(R + 10), Name8(R + 1), FMath::Max(1, S32(R + 19)) });
		}
	}
	else
	{
		for (const FAnimDef& D : GAnimDefs)
		{
			Defs.Add({ D.bFlat, D.First, D.Last, 8 });
		}
	}
	for (const FDef& D : Defs)
	{
		const TArray<FName>& Order = D.bFlat ? FlatOrder : TexOrder;
		const int32 A = Order.IndexOfByKey(FName(*D.First));
		const int32 B = Order.IndexOfByKey(FName(*D.Last));
		if (A == INDEX_NONE || B == INDEX_NONE || B <= A)
		{
			continue;
		}
		FAnimGroup G;
		G.bFlat = D.bFlat;
		G.Speed = D.Speed;
		for (int32 i = A; i <= B; ++i)
		{
			G.Frames.Add(Order[i]);
		}
		const int32 GroupIndex = Anims.Add(G);
		for (int32 i = 0; i < G.Frames.Num(); ++i)
		{
			(D.bFlat ? AnimLookupFlat : AnimLookupWall).Add(G.Frames[i], TPair<int32, int32>(GroupIndex, i));
		}
	}
	UE_LOG(LogDoom, Log, TEXT("%d animations (%s)"), Anims.Num(), Animated.Num() ? TEXT("ANIMATED lump") : TEXT("built-in table"));

	// Boom's SWITCHES lump (20-byte records: off name, on name, episode; 0 ends): pairs up to
	// the game's episode level (1 shareware, 2 registered Doom, 3 Doom 2) replace the SW1/SW2 rule.
	TConstArrayView<uint8> Switches = Wad.LumpData(TEXT("SWITCHES"));
	if (Switches.Num() > 0)
	{
		bSwitchLump = true;
		const int32 GameLevel = Wad.IsDoom2() ? 3 : 2;
		for (int32 Pos = 0; Pos + 20 <= Switches.Num(); Pos += 20)
		{
			const int32 Episode = S16(&Switches[Pos + 18]);
			if (Episode == 0)
			{
				break;
			}
			if (Episode > GameLevel)
			{
				continue;
			}
			const FName Off(*Name8(&Switches[Pos])), On(*Name8(&Switches[Pos + 9]));
			if (TexDefs.Contains(Off) && TexDefs.Contains(On))
			{
				SwitchPairs.Add(Off, On);
				SwitchPairs.Add(On, Off);
			}
		}
		UE_LOG(LogDoom, Log, TEXT("SWITCHES lump: %d switch textures"), SwitchPairs.Num());
	}
}

bool FDoomGraphics::IsAnimated(FName Name, bool bFlat) const
{
	return (bFlat ? AnimLookupFlat : AnimLookupWall).Contains(Name);
}

FName FDoomGraphics::AnimatedFrame(FName Name, bool bFlat, int32 Tic) const
{
	const TPair<int32, int32>* L = (bFlat ? AnimLookupFlat : AnimLookupWall).Find(Name);
	if (!L)
	{
		return Name;
	}
	const FAnimGroup& G = Anims[L->Key];
	return G.Frames[(L->Value + Tic / G.Speed) % G.Frames.Num()];
}

FName FDoomGraphics::SwitchPartner(FName Name) const
{
	if (bSwitchLump)
	{
		const FName* Found = SwitchPairs.Find(Name);
		return Found ? *Found : NAME_None;
	}
	const FString S = Name.ToString();
	FString Other;
	if (S.StartsWith(TEXT("SW1")))
	{
		Other = TEXT("SW2") + S.Mid(3);
	}
	else if (S.StartsWith(TEXT("SW2")))
	{
		Other = TEXT("SW1") + S.Mid(3);
	}
	else
	{
		return NAME_None;
	}
	const FName O(*Other);
	return TexDefs.Contains(O) ? O : NAME_None;
}

FName FDoomGraphics::SkyTextureFor(const FString& MapName)
{
	if (MapName.StartsWith(TEXT("MAP")))
	{
		const int32 N = FCString::Atoi(*MapName.Mid(3));
		return N < 12 ? FName(TEXT("SKY1")) : N < 21 ? FName(TEXT("SKY2")) : FName(TEXT("SKY3"));
	}
	if (MapName.Len() >= 2 && MapName[0] == 'E')
	{
		return FName(*FString::Printf(TEXT("SKY%c"), MapName[1]));
	}
	return FName(TEXT("SKY1"));
}

bool FDoomGraphics::IsSkyFlat(FName Name)
{
	static const FName Sky(TEXT("F_SKY1"));
	return Name == Sky;
}

bool FDoomGraphics::DecodePatch(TConstArrayView<uint8> Data, FDoomPicture& Out) const
{
	if (Data.Num() < 8)
	{
		return false;
	}
	const int32 W = S16(&Data[0]);
	const int32 H = S16(&Data[2]);
	if (W <= 0 || H <= 0 || W > 4096 || H > 4096 || 8 + W * 4 > Data.Num())
	{
		return false;
	}
	Out.Width = W;
	Out.Height = H;
	Out.LeftOffset = S16(&Data[4]);
	Out.TopOffset = S16(&Data[6]);
	Out.Indices.Init(-1, W * H);
	for (int32 X = 0; X < W; ++X)
	{
		int32 Ofs = S32(&Data[8 + X * 4]);
		int32 LastTop = -1;
		for (int32 Guard = 0; Guard < 256 && Ofs >= 0 && Ofs < Data.Num(); ++Guard)
		{
			int32 TopDelta = Data[Ofs];
			if (TopDelta == 0xFF)
			{
				break;
			}
			// DeePsea tall patches: a delta <= the previous one is relative
			if (TopDelta <= LastTop)
			{
				TopDelta += LastTop;
			}
			LastTop = TopDelta;
			if (Ofs + 3 >= Data.Num())
			{
				break;
			}
			const int32 Len = Data[Ofs + 1];
			const int32 Start = Ofs + 3;
			for (int32 k = 0; k < Len && Start + k < Data.Num(); ++k)
			{
				const int32 Y = TopDelta + k;
				if (Y >= 0 && Y < H)
				{
					Out.Indices[Y * W + X] = Data[Start + k];
				}
			}
			Ofs = Start + Len + 1;
		}
	}
	Out.bMasked = Out.Indices.Contains(-1);
	return true;
}

void FDoomGraphics::PrepareMips(const FDoomPicture& Pic, bool bMips, FDoomPictureMips& Out) const
{
	static const bool bNoMips = FParse::Param(FCommandLine::Get(), TEXT("nomips"));
	const TArray<FColor>& Pal = Wad.Palette(0);
	const int32 Num = Pic.Width * Pic.Height;
	Out.Color.Reset();
	Out.Index.Reset();

	// colour: the palette colour per texel
	TArray<FColor>& Pixels = Out.Color.AddDefaulted_GetRef();
	Pixels.SetNumUninitialized(Num);
	for (int32 i = 0; i < Num; ++i)
	{
		const int16 Idx = Pic.Indices[i];
		Pixels[i] = Idx < 0 ? FColor(0, 0, 0, 0) : Pal[Idx];
	}
	// Transparent texels take a neighbour's colour so bilinear/mips never fringe black.
	if (Pic.bMasked)
	{
		for (int32 Y = 0; Y < Pic.Height; ++Y)
		{
			for (int32 X = 0; X < Pic.Width; ++X)
			{
				FColor& C = Pixels[Y * Pic.Width + X];
				if (C.A == 0)
				{
					for (int32 D = 1; D < 3 && C.R + C.G + C.B == 0; ++D)
					{
						for (const FIntPoint O : { FIntPoint(D, 0), FIntPoint(-D, 0), FIntPoint(0, D), FIntPoint(0, -D) })
						{
							const int32 NX = X + O.X, NY = Y + O.Y;
							if (NX >= 0 && NY >= 0 && NX < Pic.Width && NY < Pic.Height && Pic.Indices[NY * Pic.Width + NX] >= 0)
							{
								C = Pal[Pic.Indices[NY * Pic.Width + NX]];
								C.A = 0;
								break;
							}
						}
					}
				}
			}
		}
	}
	// index: R = palette index, A = coverage; linear (not sRGB) so the index survives sampling exactly
	TArray<FColor>& Indices = Out.Index.AddDefaulted_GetRef();
	Indices.SetNumUninitialized(Num);
	for (int32 i = 0; i < Num; ++i)
	{
		const int16 Idx = Pic.Indices[i];
		Indices[i] = FColor((uint8)FMath::Max<int16>(Idx, 0), 0, 0, Idx < 0 ? 0 : 255);
	}

	// Box-filtered mip chains for power-of-two textures (walls and flats), so distant surfaces do
	// not sparkle; magnification stays nearest for crisp Doom texels. The index mips hold the
	// palette colour nearest each 2x2 block's average (indices cannot be averaged).
	if (!bMips || bNoMips || !IsPow2(Pic.Width) || !IsPow2(Pic.Height))
	{
		return;
	}
	TMap<uint32, uint8> Nearest; // per picture: this runs on worker threads
	auto NearestIndex = [&Pal, &Nearest](const FColor& C) -> uint8
	{
		const uint32 Key = (C.R << 16) | (C.G << 8) | C.B;
		if (const uint8* Found = Nearest.Find(Key))
		{
			return *Found;
		}
		int32 Best = 0, BestDist = MAX_int32;
		for (int32 i = 0; i < 256; ++i)
		{
			const int32 DR = Pal[i].R - C.R, DG = Pal[i].G - C.G, DB = Pal[i].B - C.B;
			const int32 D = DR * DR + DG * DG + DB * DB;
			if (D < BestDist) { BestDist = D; Best = i; }
		}
		Nearest.Add(Key, (uint8)Best);
		return (uint8)Best;
	};
	TArray<FColor> PrevRgb;
	PrevRgb.SetNumUninitialized(Num);
	for (int32 i = 0; i < Num; ++i)
	{
		const FColor& P = Pal[Indices[i].R];
		PrevRgb[i] = FColor(P.R, P.G, P.B, Indices[i].A);
	}
	int32 W = Pic.Width, H = Pic.Height;
	while (W > 1 || H > 1)
	{
		const int32 NW = FMath::Max(1, W / 2), NH = FMath::Max(1, H / 2);
		const TArray<FColor>& Prev = Out.Color.Last();
		TArray<FColor> Next, NextRgb, NextIndex;
		Next.SetNumUninitialized(NW * NH);
		NextRgb.SetNumUninitialized(NW * NH);
		NextIndex.SetNumUninitialized(NW * NH);
		for (int32 Y = 0; Y < NH; ++Y)
		{
			for (int32 X = 0; X < NW; ++X)
			{
				int32 R = 0, G = 0, B = 0, A = 0, IR = 0, IG = 0, IB = 0, IA = 0, N = 0;
				for (int32 dy = 0; dy < 2; ++dy)
				{
					for (int32 dx = 0; dx < 2; ++dx)
					{
						const int32 i = FMath::Min(Y * 2 + dy, H - 1) * W + FMath::Min(X * 2 + dx, W - 1);
						const FColor& C = Prev[i];
						R += C.R; G += C.G; B += C.B; A += C.A;
						const FColor& P = PrevRgb[i];
						IR += P.R; IG += P.G; IB += P.B; IA += P.A;
						++N;
					}
				}
				// alpha: keep coverage stable for masked textures (majority vote)
				Next[Y * NW + X] = FColor(R / N, G / N, B / N, A * 2 >= 255 * N ? 255 : 0);
				const FColor Avg(IR / N, IG / N, IB / N, IA * 2 >= 255 * N ? 255 : 0);
				NextRgb[Y * NW + X] = Avg;
				NextIndex[Y * NW + X] = FColor(NearestIndex(Avg), 0, 0, Avg.A);
			}
		}
		Out.Color.Add(MoveTemp(Next));
		Out.Index.Add(MoveTemp(NextIndex));
		PrevRgb = MoveTemp(NextRgb);
		W = NW;
		H = NH;
	}
}

UTexture2D* FDoomGraphics::CreateTexture(int32 Width, int32 Height, const TArray<TArray<FColor>>& Mips, bool bSRGB, bool bWrap, FName Name)
{
	UTexture2D* Tex = NewTexture(Width, Height, Mips[0], Name);
	if (!Tex)
	{
		return nullptr;
	}
	// The platform data's mip count sizes the GPU texture; the pixels themselves are uploaded
	// below, since a transient texture's resource only initializes mip 0.
	FTexturePlatformData* PD = Tex->GetPlatformData();
	for (int32 m = 1; m < Mips.Num(); ++m)
	{
		const int32 MW = FMath::Max(1, Width >> m), MH = FMath::Max(1, Height >> m);
		FTexture2DMipMap* Mip = new FTexture2DMipMap(MW, MH, 1);
		PD->Mips.Add(Mip);
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Mip->BulkData.Realloc((int64)MW * MH * 4), Mips[m].GetData(), (int64)MW * MH * 4);
		Mip->BulkData.Unlock();
	}
	Tex->Filter = TF_Nearest;
	Tex->SRGB = bSRGB;
	Tex->AddressX = Tex->AddressY = bWrap ? TA_Wrap : TA_Clamp;
	// Pixels2D is never LOD-biased: a World-group bias could leave a one-mip texture with no resident mip
	Tex->LODGroup = TEXTUREGROUP_Pixels2D;
	Tex->NeverStream = true;
	Tex->UpdateResource();
	for (int32 m = 1; m < Mips.Num(); ++m)
	{
		const int32 MW = FMath::Max(1, Width >> m), MH = FMath::Max(1, Height >> m);
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, MW, MH);
		uint8* Copy = (uint8*)FMemory::Malloc((SIZE_T)MW * MH * 4);
		FMemory::Memcpy(Copy, Mips[m].GetData(), (SIZE_T)MW * MH * 4);
		Tex->UpdateTextureRegions(m, 1, Region, MW * 4, 4, Copy,
			[](uint8* Data, const FUpdateTextureRegion2D* Regions)
			{
				FMemory::Free(Data);
				delete Regions;
			});
	}
	Owned.Add(Tex);
	return Tex;
}

void FDoomGraphics::CreateTextures(FDoomPicture& Pic, const FDoomPictureMips& Mips, bool bWrap)
{
	const FName ObjName = MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), FName(*FString::Printf(TEXT("Doom_%s"), *Pic.Name.ToString())));
	Pic.Texture = CreateTexture(Pic.Width, Pic.Height, Mips.Color, true, bWrap, ObjName);
	Pic.IndexTexture = Pic.Texture ? CreateTexture(Pic.Width, Pic.Height, Mips.Index, false, bWrap, NAME_None) : nullptr;
}

void FDoomGraphics::Upload(FDoomPicture& Pic, bool bWrap, bool bMips)
{
	FDoomPictureMips Mips;
	PrepareMips(Pic, bMips, Mips);
	CreateTextures(Pic, Mips, bWrap);
}

TSharedRef<FDoomPicturePrefetch> FDoomGraphics::CollectPrefetch(const FDoomMap& Map) const
{
	TSharedRef<FDoomPicturePrefetch> P = MakeShared<FDoomPicturePrefetch>();
	TSet<FName> Walls, Flats;
	auto AddWithFrames = [this](TSet<FName>& Set, FName Name, bool bFlat)
	{
		if (Name.IsNone() || (bFlat && IsSkyFlat(Name)))
		{
			return;
		}
		Set.Add(Name);
		// animation frames and switch partners: no decoding hitch when they first show
		if (const TPair<int32, int32>* Anim = (bFlat ? AnimLookupFlat : AnimLookupWall).Find(Name))
		{
			Set.Append(Anims[Anim->Key].Frames);
		}
		if (!bFlat)
		{
			const FName Partner = SwitchPartner(Name);
			if (!Partner.IsNone()) Set.Add(Partner);
		}
	};
	for (const FDoomSide& S : Map.Sides)
	{
		AddWithFrames(Walls, S.Upper, false);
		AddWithFrames(Walls, S.Middle, false);
		AddWithFrames(Walls, S.Lower, false);
	}
	for (const FDoomSector& S : Map.Sectors)
	{
		AddWithFrames(Flats, S.FloorPic, true);
		AddWithFrames(Flats, S.CeilingPic, true);
	}
	for (const FName Name : Walls)
	{
		if (!TextureCache.Contains(Name)) P->Items.Add({ Name, false });
	}
	for (const FName Name : Flats)
	{
		if (!FlatCache.Contains(Name)) P->Items.Add({ Name, true });
	}
	return P;
}

void FDoomGraphics::DecodePrefetch(FDoomPicturePrefetch& P) const
{
	ParallelFor(P.Items.Num(), [this, &P](int32 i)
	{
		FDoomPicturePrefetch::FItem& Item = P.Items[i];
		TSharedPtr<FDoomPicture> Pic = MakeShared<FDoomPicture>();
		if (Item.bFlat ? ComposeFlat(Item.Name, *Pic) : ComposeTexture(Item.Name, *Pic))
		{
			PrepareMips(*Pic, true, Item.Mips);
			Item.Pic = Pic;
		}
	});
}

void FDoomGraphics::CachePrefetch(FDoomPicturePrefetch& P)
{
	for (FDoomPicturePrefetch::FItem& Item : P.Items)
	{
		(Item.bFlat ? FlatCache : TextureCache).Add(Item.Name, Item.Pic);
	}
}

bool FDoomGraphics::CreatePrefetchTextures(FDoomPicturePrefetch& P, double Deadline)
{
	while (P.NextToCreate < P.Items.Num())
	{
		FDoomPicturePrefetch::FItem& Item = P.Items[P.NextToCreate++];
		if (Item.Pic.IsValid())
		{
			CreateTextures(*Item.Pic, Item.Mips, true);
			Item.Mips = FDoomPictureMips();
		}
		if (FPlatformTime::Seconds() > Deadline)
		{
			break;
		}
	}
	return P.NextToCreate >= P.Items.Num();
}

UTexture2D* FDoomGraphics::ColormappedPicture(const FDoomPicture& Pic, int32 Row, int32 Palette)
{
	Palette = FMath::Clamp(Palette, 0, FMath::Max(0, Wad.NumPalettes() - 1));
	const TPair<const FDoomPicture*, int32> Key(&Pic, Row * 16 + Palette);
	if (TObjectPtr<UTexture2D>* Found = ColormappedCache.Find(Key))
	{
		return *Found;
	}
	UTexture2D* Tex = MakeTextureFromIndices(Pic, FMath::Clamp(Row, 0, 33), Palette);
	ColormappedCache.Add(Key, Tex);
	return Tex;
}

const FDoomPicture* FDoomGraphics::Texture(FName Name)
{
	if (Name.IsNone())
	{
		return nullptr;
	}
	if (TSharedPtr<FDoomPicture>* Cached = TextureCache.Find(Name))
	{
		return Cached->Get();
	}
	TSharedPtr<FDoomPicture> Pic = MakeShared<FDoomPicture>();
	if (!ComposeTexture(Name, *Pic))
	{
		TextureCache.Add(Name, nullptr);
		return nullptr;
	}
	Upload(*Pic, true, true);
	const FDoomPicture* Result = Pic.Get();
	TextureCache.Add(Name, MoveTemp(Pic));
	return Result;
}

bool FDoomGraphics::ComposeTexture(FName Name, FDoomPicture& Out) const
{
	const FTexDef* Def = TexDefs.Find(Name);
	if (!Def || Def->Width <= 0 || Def->Height <= 0)
	{
		return false;
	}
	FDoomPicture* Pic = &Out;
	Pic->Name = Name;
	Pic->Width = Def->Width;
	Pic->Height = Def->Height;
	Pic->Indices.Init(-1, Def->Width * Def->Height);
	for (const FTexDef::FPatchRef& PR : Def->Patches)
	{
		if (!PNames.IsValidIndex(PR.PNameIndex))
		{
			continue;
		}
		FDoomPicture Patch;
		if (!DecodePatch(Wad.LumpData(PNames[PR.PNameIndex]), Patch))
		{
			continue;
		}
		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			const int32 TY = PR.Y + Y;
			if (TY < 0 || TY >= Pic->Height)
			{
				continue;
			}
			for (int32 X = 0; X < Patch.Width; ++X)
			{
				const int32 TX = PR.X + X;
				const int16 Idx = Patch.Indices[Y * Patch.Width + X];
				if (TX >= 0 && TX < Pic->Width && Idx >= 0)
				{
					Pic->Indices[TY * Pic->Width + TX] = Idx;
				}
			}
		}
	}
	Pic->bMasked = Pic->Indices.Contains(-1);
	return true;
}

int32 FDoomGraphics::TextureHeight(FName Name)
{
	const FTexDef* Def = TexDefs.Find(Name);
	return Def ? Def->Height : 0;
}

const FDoomPicture* FDoomGraphics::Flat(FName Name)
{
	if (Name.IsNone())
	{
		return nullptr;
	}
	if (TSharedPtr<FDoomPicture>* Cached = FlatCache.Find(Name))
	{
		return Cached->Get();
	}
	TSharedPtr<FDoomPicture> Pic = MakeShared<FDoomPicture>();
	if (!ComposeFlat(Name, *Pic))
	{
		FlatCache.Add(Name, nullptr);
		return nullptr;
	}
	Upload(*Pic, true, true);
	const FDoomPicture* Result = Pic.Get();
	FlatCache.Add(Name, MoveTemp(Pic));
	return Result;
}

bool FDoomGraphics::ComposeFlat(FName Name, FDoomPicture& Out) const
{
	const int32* Lump = FlatLumps.Find(Name);
	if (!Lump)
	{
		return false;
	}
	TConstArrayView<uint8> D = Wad.LumpData(*Lump);
	Out.Name = Name;
	Out.Width = Out.Height = 64;
	Out.Indices.SetNum(4096);
	for (int32 i = 0; i < 4096; ++i)
	{
		Out.Indices[i] = i < D.Num() ? D[i] : 0;
	}
	return true;
}

const FDoomPicture* FDoomGraphics::Patch(const FString& LumpName)
{
	const FName Key(*LumpName);
	if (TSharedPtr<FDoomPicture>* Cached = PatchCache.Find(Key))
	{
		return Cached->Get();
	}
	TSharedPtr<FDoomPicture> Pic = MakeShared<FDoomPicture>();
	Pic->Name = Key;
	if (!DecodePatch(Wad.LumpData(LumpName), *Pic))
	{
		PatchCache.Add(Key, nullptr);
		return nullptr;
	}
	Upload(*Pic, false, false);
	const FDoomPicture* Result = Pic.Get();
	PatchCache.Add(Key, MoveTemp(Pic));
	return Result;
}

bool FDoomGraphics::SpriteHasRotations(const FString& InPrefix, TCHAR Frame) const
{
	const FString Prefix = DoomInfo::SpriteName(InPrefix);
	return !SpriteLumps.Contains(Prefix + Frame + TEXT('0'));
}

FDoomSpriteFrame FDoomGraphics::SpriteFrame(const FString& InPrefix, TCHAR Frame, int32 Rotation, int32 Translation)
{
	const FString Prefix = DoomInfo::SpriteName(InPrefix);
	FDoomSpriteFrame Result;
	const FSpriteLump* L = SpriteLumps.Find(Prefix + Frame + TEXT('0'));
	if (!L)
	{
		L = SpriteLumps.Find(Prefix + Frame + TCHAR('0' + FMath::Clamp(Rotation, 1, 8)));
	}
	if (!L)
	{
		L = SpriteLumps.Find(Prefix + Frame + TEXT('1'));
	}
	if (!L)
	{
		return Result;
	}
	TSharedPtr<FDoomPicture>* Cached = SpriteCache.Find(L->Lump);
	if (!Cached)
	{
		TSharedPtr<FDoomPicture> Pic = MakeShared<FDoomPicture>();
		Pic->Name = Wad.Lump(L->Lump).Name;
		if (DecodePatch(Wad.LumpData(L->Lump), *Pic))
		{
			Upload(*Pic, false, false);
		}
		Cached = &SpriteCache.Add(L->Lump, MoveTemp(Pic));
	}
	if (Cached->Get() && (*Cached)->IsValid())
	{
		Result.Picture = Cached->Get();
		Result.bMirror = L->bMirror;
		if (Translation >= 1 && Translation <= 3)
		{
			const int64 Key = ((int64)L->Lump << 2) | Translation;
			TSharedPtr<FDoomPicture>* T = TranslatedSprites.Find(Key);
			if (!T)
			{
				// the green ramp onto grey, brown or red, as Doom's translation tables do
				static const int16 Bases[4] = { 0x70, 0x60, 0x40, 0x20 };
				TSharedPtr<FDoomPicture> Pic = MakeShared<FDoomPicture>(**Cached);
				Pic->Texture = nullptr;
				Pic->IndexTexture = nullptr;
				for (int16& I : Pic->Indices)
				{
					if (I >= 0x70 && I <= 0x7F) I = Bases[Translation] + (I & 0x0F);
				}
				Upload(*Pic, false, false);
				T = &TranslatedSprites.Add(Key, MoveTemp(Pic));
			}
			Result.Picture = T->Get();
		}
	}
	return Result;
}

int32 FDoomGraphics::ColormapRows() const
{
	return 34 * FMath::Max(1, Wad.NumPalettes());
}

UTexture2D* FDoomGraphics::ColormapTexture()
{
	if (Colormap)
	{
		return Colormap;
	}
	// 256 x (34 x palettes): row p * 34 + r = COLORMAP row r mapped through PLAYPAL palette p,
	// so the shaders can follow Doom's damage / pickup / radiation suit palettes exactly.
	const TArray<uint8>& Cm = Wad.Colormap();
	const int32 NumPal = FMath::Max(1, Wad.NumPalettes());
	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(256 * 34 * NumPal);
	for (int32 p = 0; p < NumPal; ++p)
	{
		const TArray<FColor>& Pal = Wad.Palette(p);
		for (int32 i = 0; i < 256 * 34; ++i)
		{
			Pixels[p * 256 * 34 + i] = Pal[Cm.IsValidIndex(i) ? Cm[i] : i % 256];
		}
	}
	Colormap = NewTexture(256, 34 * NumPal, Pixels, TEXT("Doom_COLORMAP"));
	Colormap->Filter = TF_Nearest;
	Colormap->SRGB = true;
	Colormap->AddressX = Colormap->AddressY = TA_Clamp;
	Colormap->UpdateResource();
	return Colormap;
}

UTexture2D* FDoomGraphics::MakeTextureFromIndices(const FDoomPicture& Pic, int32 ColormapRow, int32 PaletteIndex)
{
	const TArray<FColor>& Pal = Wad.Palette(PaletteIndex);
	const TArray<uint8>& Cm = Wad.Colormap();
	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(Pic.Width * Pic.Height);
	for (int32 i = 0; i < Pixels.Num(); ++i)
	{
		const int16 Idx = Pic.Indices[i];
		if (Idx < 0)
		{
			Pixels[i] = FColor(0, 0, 0, 0);
			continue;
		}
		const int32 Mapped = Cm.IsValidIndex(ColormapRow * 256 + Idx) ? Cm[ColormapRow * 256 + Idx] : Idx;
		Pixels[i] = Pal[Mapped];
	}
	UTexture2D* Tex = NewTexture(Pic.Width, Pic.Height, Pixels, NAME_None);
	if (Tex)
	{
		Tex->Filter = TF_Nearest;
		Tex->SRGB = true;
		Tex->UpdateResource();
	}
	return Tex;
}

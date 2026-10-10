#include "DoomLevel.h"
#include "DoomMap.h"
#include "DoomGraphics.h"
#include "DoomAssets.h"
#include "DoomUE.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

namespace
{
	constexpr float KindWall = 0.f;
	constexpr float KindPlane = 0.5f;
	constexpr float KindFullBright = 1.f;

	float Contrast(const FVector2f& A, const FVector2f& B)
	{
		// Doom's "fake contrast": east-west walls darker, north-south walls brighter.
		if (A.Y == B.Y) return -16.f;
		if (A.X == B.X) return 16.f;
		return 0.f;
	}
}

ADoomLevel::ADoomLevel()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

UProceduralMeshComponent* ADoomLevel::NewMeshComponent(const FString& Name, bool bCollision)
{
	UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(this, FName(*Name));
	C->SetupAttachment(RootComponent);
	C->bUseAsyncCooking = true;
	C->bUseComplexAsSimpleCollision = true;
	C->SetCastShadow(bModern);
	C->bAffectDistanceFieldLighting = true;
	if (bCollision)
	{
		C->SetCollisionProfileName(TEXT("BlockAll"));
		C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}
	else
	{
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	C->RegisterComponent();
	MeshComponents.Add(C);
	return C;
}

void ADoomLevel::Build(FDoomMap& InMap, FDoomGraphics& InGfx, const TSet<int32>& MovableSectors, bool bInModern, FName SkyTexture)
{
	Map = &InMap;
	Gfx = &InGfx;
	bModern = bInModern;
	WorldMaterial = DoomAssets::WorldMaterial(bModern);
	SkyMaterial = DoomAssets::SkyMaterial();
	SkyKey = SectionKey(SkyTexture, false, true);

	// sector light texture: 256 sectors per row
	SectorLightRows = FMath::Max(1, FMath::DivideAndRoundUp(Map->Sectors.Num(), 256));
	SectorLightPixels.SetNumZeroed(256 * SectorLightRows);
	SectorLightTex = UTexture2D::CreateTransient(256, SectorLightRows, PF_B8G8R8A8, TEXT("Doom_SectorLights"));
	SectorLightTex->Filter = TF_Nearest;
	SectorLightTex->SRGB = false;
	SectorLightTex->AddressX = SectorLightTex->AddressY = TA_Clamp;
	SectorLightTex->NeverStream = true;
	SectorLightTex->UpdateResource();
	UpdateSectorLights();

	// chunks: 0 = static, then one per movable sector, then one per switch line
	Chunks.Reset();
	Chunks.AddDefaulted();
	TArray<int32> SectorToChunk;
	SectorToChunk.Init(0, Map->Sectors.Num());
	SectorChunks.Reset();
	SectorChunks.SetNum(Map->Sectors.Num());
	for (int32 s = 0; s < Map->Sectors.Num(); ++s)
	{
		if (MovableSectors.Contains(s))
		{
			SectorToChunk[s] = Chunks.Num();
			FChunk& C = Chunks.AddDefaulted_GetRef();
			C.bDynamic = true;
		}
		Chunks[SectorToChunk[s]].Sectors.Add(s);
		SectorChunks[s].AddUnique(SectorToChunk[s]);
	}
	LineChunk.Init(0, Map->Lines.Num());
	for (int32 l = 0; l < Map->Lines.Num(); ++l)
	{
		const FDoomLine& L = Map->Lines[l];
		int32 Chunk = 0;
		if (L.FrontSector != INDEX_NONE && SectorToChunk[L.FrontSector] != 0)
		{
			Chunk = SectorToChunk[L.FrontSector];
		}
		else if (L.BackSector != INDEX_NONE && SectorToChunk[L.BackSector] != 0)
		{
			Chunk = SectorToChunk[L.BackSector];
		}
		else if (L.Special != 0)
		{
			// switch textures flip: keep the line in its own small chunk
			bool bSwitch = false;
			for (int32 s = 0; s < 2; ++s)
			{
				if (L.Side[s] == INDEX_NONE) continue;
				const FDoomSide& Sd = Map->Sides[L.Side[s]];
				bSwitch |= !Gfx->SwitchPartner(Sd.Upper).IsNone() || !Gfx->SwitchPartner(Sd.Middle).IsNone() || !Gfx->SwitchPartner(Sd.Lower).IsNone();
			}
			if (bSwitch)
			{
				Chunk = Chunks.Num();
				Chunks.AddDefaulted();
			}
		}
		LineChunk[l] = Chunk;
		Chunks[Chunk].Lines.Add(l);
		for (int32 Sec : { L.FrontSector, L.BackSector })
		{
			if (Sec != INDEX_NONE)
			{
				SectorChunks[Sec].AddUnique(Chunk);
			}
		}
	}

	for (int32 i = 0; i < Chunks.Num(); ++i)
	{
		Chunks[i].Solid = NewMeshComponent(FString::Printf(TEXT("Chunk%d_Solid"), i), true);
		Chunks[i].NonSolid = NewMeshComponent(FString::Printf(TEXT("Chunk%d_Masked"), i), false);
		Chunks[i].bDirty = true;
	}
	Flush();
	BuildBlockingCollision();
	UE_LOG(LogDoom, Log, TEXT("Level geometry: %d chunks, %d materials"), Chunks.Num(), Materials.Num());
}

FName ADoomLevel::SectionKey(FName Texture, bool bFlat, bool bSky) const
{
	if (bSky)
	{
		return FName(*(TEXT("S:") + Texture.ToString()));
	}
	return FName(*((bFlat ? TEXT("F:") : TEXT("W:")) + Texture.ToString()));
}

UMaterialInstanceDynamic* ADoomLevel::GetSurfaceMaterial(FName Texture, bool bFlat)
{
	return Cast<UMaterialInstanceDynamic>(MaterialForKey(SectionKey(Texture, bFlat, false)));
}

UMaterialInterface* ADoomLevel::MaterialForKey(FName Key)
{
	if (TObjectPtr<UMaterialInstanceDynamic>* Found = Materials.Find(Key))
	{
		return *Found;
	}
	const FString K = Key.ToString();
	const FName TexName(*K.Mid(2));
	UMaterialInstanceDynamic* MID = nullptr;
	if (K.StartsWith(TEXT("S:")))
	{
		MID = UMaterialInstanceDynamic::Create(SkyMaterial, this);
		const FDoomPicture* Pic = Gfx->Texture(TexName);
		if (Pic && Pic->Texture)
		{
			MID->SetTextureParameterValue(TEXT("Tex"), Pic->Texture);
			MID->SetScalarParameterValue(TEXT("SkyHeight"), Pic->Height);
		}
	}
	else
	{
		MID = UMaterialInstanceDynamic::Create(WorldMaterial, this);
		const bool bFlat = K.StartsWith(TEXT("F:"));
		const FDoomPicture* Pic = bFlat ? Gfx->Flat(TexName) : Gfx->Texture(TexName);
		if (Pic && Pic->Texture)
		{
			MID->SetTextureParameterValue(TEXT("Tex"), Pic->Texture);
			if (K.StartsWith(TEXT("X:")))
			{
				MID->SetScalarParameterValue(TEXT("ScrollU"), 1.f / Pic->Width);
			}
		}
		MID->SetTextureParameterValue(TEXT("SectorLights"), SectorLightTex);
		MID->SetScalarParameterValue(TEXT("SectorLightRows"), SectorLightRows);
	}
	Materials.Add(Key, MID);
	return MID;
}

void ADoomLevel::EmitQuad(FSectionData& S, const FVector2f& A, const FVector2f& B, float Bottom, float Top,
	float U0, float U1, float TexTop, float TexW, float TexH, int32 Sector, float ContrastLight, float Kind)
{
	const int32 Base = S.V.Num();
	const FVector2f D = B - A;
	const FVector Normal = FVector(D.Y, D.X, 0).GetSafeNormal();
	const float VBottom = (TexTop - Bottom) / TexH;
	const float VTop = (TexTop - Top) / TexH;
	S.V.Add(Doom::ToUE(A, Bottom)); S.UV0.Add(FVector2D(U0 / TexW, VBottom));
	S.V.Add(Doom::ToUE(A, Top));    S.UV0.Add(FVector2D(U0 / TexW, VTop));
	S.V.Add(Doom::ToUE(B, Top));    S.UV0.Add(FVector2D(U1 / TexW, VTop));
	S.V.Add(Doom::ToUE(B, Bottom)); S.UV0.Add(FVector2D(U1 / TexW, VBottom));
	for (int32 k = 0; k < 4; ++k)
	{
		S.N.Add(Normal);
		S.UV1.Add(FVector2D(Sector, ContrastLight));
		S.C.Add(FLinearColor(Kind, 0, 0, 1));
	}
	S.I.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
}

void ADoomLevel::EmitPlanes(int32 SectorIndex, TMap<FName, FSectionData>& Out)
{
	const FDoomSector& Sec = Map->Sectors[SectorIndex];
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bCeiling = Pass == 1;
		const FName Pic = bCeiling ? Sec.CeilingPic : Sec.FloorPic;
		const bool bSky = FDoomGraphics::IsSkyFlat(Pic);
		const FName Key = bSky ? SkyKey : SectionKey(Pic, true, false);
		if (!bSky && !Gfx->Flat(Pic))
		{
			continue;
		}
		FSectionData& S = Out.FindOrAdd(Key);
		const float Z = bCeiling ? Sec.Ceiling : Sec.Floor;
		const FVector Normal(0, 0, bCeiling ? -1 : 1);
		for (int32 SsIndex : Sec.Subsectors)
		{
			const TArray<FVector2f>& Poly = Map->Subsectors[SsIndex].Polygon;
			if (Poly.Num() < 3)
			{
				continue;
			}
			const int32 Base = S.V.Num();
			for (const FVector2f& P : Poly)
			{
				S.V.Add(Doom::ToUE(P, Z));
				S.N.Add(Normal);
				S.UV0.Add(FVector2D(P.X / 64.f, -P.Y / 64.f));
				S.UV1.Add(FVector2D(SectorIndex, 0));
				S.C.Add(FLinearColor(bSky ? KindFullBright : KindPlane, 0, 0, 1));
			}
			for (int32 k = 1; k + 1 < Poly.Num(); ++k)
			{
				if (bCeiling)
				{
					S.I.Append({ Base, Base + k, Base + k + 1 });
				}
				else
				{
					S.I.Append({ Base, Base + k + 1, Base + k });
				}
			}
		}
	}
}

void ADoomLevel::EmitLine(int32 LineIndex, bool bDynamic, TMap<FName, FSectionData>& OutSolid, TMap<FName, FSectionData>& OutNonSolid)
{
	const FDoomLine& L = Map->Lines[LineIndex];
	const float Len = FVector2f::Distance(Map->V(L.V1), Map->V(L.V2));
	const float ContrastLight = Contrast(Map->V(L.V1), Map->V(L.V2));
	const bool bTwoSided = L.Side[0] != INDEX_NONE && L.Side[1] != INDEX_NONE;

	for (int32 SideIdx = 0; SideIdx < 2; ++SideIdx)
	{
		if (L.Side[SideIdx] == INDEX_NONE)
		{
			continue;
		}
		const FDoomSide& Side = Map->Sides[L.Side[SideIdx]];
		const FVector2f A = Map->V(SideIdx == 0 ? L.V1 : L.V2);
		const FVector2f B = Map->V(SideIdx == 0 ? L.V2 : L.V1);
		const FDoomSector& S = Map->Sectors[Side.Sector];
		const float U0 = Side.XOffset;
		const float U1 = Side.XOffset + Len;

		auto Wall = [&](FName Tex, float Bottom, float Top, float TexTop, bool bMasked)
		{
			if (Tex.IsNone() || (!bDynamic && Top <= Bottom))
			{
				return;
			}
			const FDoomPicture* Pic = Gfx->Texture(Tex);
			if (!Pic)
			{
				return;
			}
			// scrolling walls (special 48) get their own material instance with a texture scroll
			const FName Key = L.Special == 48 ? FName(*(TEXT("X:") + Tex.ToString())) : SectionKey(Tex, false, false);
			FSectionData& Sd = (bMasked ? OutNonSolid : OutSolid).FindOrAdd(Key);
			EmitQuad(Sd, A, B, Bottom, FMath::Max(Top, Bottom), U0, U1, TexTop, Pic->Width, Pic->Height, Side.Sector, ContrastLight, KindWall);
		};

		if (!bTwoSided)
		{
			const float H = Gfx->TextureHeight(Side.Middle);
			const float TexTop = ((L.Flags & DoomLineFlags::DontPegBottom) ? S.Floor + H : S.Ceiling) + Side.YOffset;
			Wall(Side.Middle, S.Floor, S.Ceiling, TexTop, false);
			continue;
		}

		const FDoomSector& O = Map->Sectors[Map->Sides[L.Side[SideIdx ^ 1]].Sector];
		// upper
		if (O.Ceiling < S.Ceiling || bDynamic)
		{
			if (FDoomGraphics::IsSkyFlat(S.CeilingPic) && FDoomGraphics::IsSkyFlat(O.CeilingPic))
			{
				// Doom leaves this gap undrawn and the sky shows through: draw it with the sky.
				if (O.Ceiling < S.Ceiling || bDynamic)
				{
					FSectionData& Sd = OutSolid.FindOrAdd(SkyKey);
					EmitQuad(Sd, A, B, O.Ceiling, FMath::Max(O.Ceiling, S.Ceiling), U0, U1, 0, 256, 128, Side.Sector, 0, KindFullBright);
				}
			}
			else
			{
				const float H = Gfx->TextureHeight(Side.Upper);
				const float TexTop = ((L.Flags & DoomLineFlags::DontPegTop) ? S.Ceiling : O.Ceiling + H) + Side.YOffset;
				Wall(Side.Upper, O.Ceiling, S.Ceiling, TexTop, false);
			}
		}
		// lower
		if (O.Floor > S.Floor || bDynamic)
		{
			const float TexTop = ((L.Flags & DoomLineFlags::DontPegBottom) ? S.Ceiling : O.Floor) + Side.YOffset;
			Wall(Side.Lower, S.Floor, O.Floor, TexTop, false);
		}
		// masked middle: one texture high, clipped to the opening
		if (!Side.Middle.IsNone())
		{
			const float H = Gfx->TextureHeight(Side.Middle);
			const float OpenTop = FMath::Min(S.Ceiling, O.Ceiling);
			const float OpenBottom = FMath::Max(S.Floor, O.Floor);
			const float TexTop = ((L.Flags & DoomLineFlags::DontPegBottom) ? OpenBottom + H : OpenTop) + Side.YOffset;
			const float Bottom = FMath::Max(OpenBottom, TexTop - H);
			const float Top = FMath::Min(OpenTop, TexTop);
			Wall(Side.Middle, Bottom, Top, TexTop, true);
		}
	}
}

void ADoomLevel::Generate(FChunk& Chunk, TMap<FName, FSectionData>& OutSolid, TMap<FName, FSectionData>& OutNonSolid)
{
	for (int32 Sec : Chunk.Sectors)
	{
		EmitPlanes(Sec, OutSolid);
	}
	for (int32 Line : Chunk.Lines)
	{
		EmitLine(Line, Chunk.bDynamic, OutSolid, OutNonSolid);
	}
}

void ADoomLevel::Commit(FChunk& Chunk, UProceduralMeshComponent* Comp, TMap<FName, int32>& Sections, TArray<int32>& Counts, TMap<FName, FSectionData>& Data, bool bCollision)
{
	TArray<FProcMeshTangent> NoTangents;
	TArray<FVector2D> Empty;
	// sections that disappeared
	for (auto It = Sections.CreateIterator(); It; ++It)
	{
		if (!Data.Contains(It.Key()))
		{
			Comp->ClearMeshSection(It.Value());
			Counts[It.Value()] = -1;
		}
	}
	for (auto& Pair : Data)
	{
		FSectionData& D = Pair.Value;
		if (D.I.Num() == 0)
		{
			continue;
		}
		int32* Existing = Sections.Find(Pair.Key);
		const int32 Signature = D.V.Num() * 7919 + D.I.Num();
		if (Existing && Counts.IsValidIndex(*Existing) && Counts[*Existing] == Signature)
		{
			Comp->UpdateMeshSection_LinearColor(*Existing, D.V, D.N, D.UV0, D.UV1, Empty, Empty, D.C, NoTangents, false);
			continue;
		}
		int32 Index;
		if (Existing)
		{
			Index = *Existing;
		}
		else
		{
			Index = Counts.Num();
			Counts.Add(-1);
			Sections.Add(Pair.Key, Index);
		}
		Comp->CreateMeshSection_LinearColor(Index, D.V, D.I, D.N, D.UV0, D.UV1, Empty, Empty, D.C, NoTangents, bCollision, false);
		Comp->SetMaterial(Index, MaterialForKey(Pair.Key));
		Counts[Index] = Signature;
	}
}

void ADoomLevel::SectorChanged(int32 Sector)
{
	if (SectorChunks.IsValidIndex(Sector))
	{
		for (int32 C : SectorChunks[Sector])
		{
			Chunks[C].bDirty = true;
		}
	}
}

void ADoomLevel::LineChanged(int32 Line)
{
	if (LineChunk.IsValidIndex(Line))
	{
		Chunks[LineChunk[Line]].bDirty = true;
	}
}

void ADoomLevel::Flush()
{
	for (FChunk& C : Chunks)
	{
		if (!C.bDirty)
		{
			continue;
		}
		C.bDirty = false;
		TMap<FName, FSectionData> Solid, NonSolid;
		Generate(C, Solid, NonSolid);
		Commit(C, C.Solid, C.SolidSections, C.SolidCounts, Solid, true);
		Commit(C, C.NonSolid, C.NonSolidSections, C.NonSolidCounts, NonSolid, false);
	}
}

void ADoomLevel::BuildBlockingCollision()
{
	float MinZ = 0, MaxZ = 0;
	for (const FDoomSector& S : Map->Sectors)
	{
		MinZ = FMath::Min(MinZ, S.Floor);
		MaxZ = FMath::Max(MaxZ, S.Ceiling);
	}
	MinZ -= 512;
	MaxZ += 512;

	FSectionData All, Monsters;
	for (const FDoomLine& L : Map->Lines)
	{
		if (L.Side[1] == INDEX_NONE)
		{
			continue; // one-sided lines are already solid walls
		}
		FSectionData* Target = (L.Flags & DoomLineFlags::Blocking) ? &All : (L.Flags & DoomLineFlags::BlockMonsters) ? &Monsters : nullptr;
		if (!Target)
		{
			continue;
		}
		const FVector2f A = Map->V(L.V1), B = Map->V(L.V2);
		EmitQuad(*Target, A, B, MinZ, MaxZ, 0, 1, 0, 1, 1, 0, 0, 0);
		EmitQuad(*Target, B, A, MinZ, MaxZ, 0, 1, 0, 1, 1, 0, 0, 0);
	}
	TArray<FProcMeshTangent> NoTangents;
	if (All.I.Num())
	{
		UProceduralMeshComponent* C = NewMeshComponent(TEXT("BlockingLines"), true);
		C->CreateMeshSection_LinearColor(0, All.V, All.I, All.N, All.UV0, All.C, NoTangents, true);
		C->SetVisibility(false);
		C->SetCastShadow(false);
		// bullets and sight pass through impassable lines; bodies do not
		C->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
		C->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		C->SetCollisionResponseToChannel(ECC_DoomMissile, ECR_Ignore);
	}
	if (Monsters.I.Num())
	{
		UProceduralMeshComponent* C = NewMeshComponent(TEXT("MonsterBlockingLines"), true);
		C->CreateMeshSection_LinearColor(0, Monsters.V, Monsters.I, Monsters.N, Monsters.UV0, Monsters.C, NoTangents, true);
		C->SetVisibility(false);
		C->SetCastShadow(false);
		C->SetCollisionResponseToAllChannels(ECR_Ignore);
		C->SetCollisionResponseToChannel(ECC_DoomMonster, ECR_Block);
	}
}

void ADoomLevel::UpdateSectorLights()
{
	if (!SectorLightTex || !Map)
	{
		return;
	}
	for (int32 i = 0; i < Map->Sectors.Num(); ++i)
	{
		const uint8 L = (uint8)FMath::Clamp<int32>(Map->Sectors[i].Light, 0, 255);
		SectorLightPixels[i] = FColor(L, L, L, 255);
	}
	const int32 W = 256, H = SectorLightRows;
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, W, H);
	uint8* Copy = (uint8*)FMemory::Malloc(W * H * 4);
	FMemory::Memcpy(Copy, SectorLightPixels.GetData(), W * H * 4);
	SectorLightTex->UpdateTextureRegions(0, 1, Region, W * 4, 4, Copy,
		[](uint8* Data, const FUpdateTextureRegion2D* Regions)
		{
			FMemory::Free(Data);
			delete Regions;
		});
}

void ADoomLevel::AnimateTextures(int32 Tic)
{
	if (Tic % 8 != 0)
	{
		return;
	}
	for (auto& Pair : Materials)
	{
		const FString K = Pair.Key.ToString();
		const bool bFlat = K.StartsWith(TEXT("F:"));
		if (!bFlat && !K.StartsWith(TEXT("W:")))
		{
			continue;
		}
		const FName Base(*K.Mid(2));
		if (!Gfx->IsAnimated(Base, bFlat))
		{
			continue;
		}
		const FName Frame = Gfx->AnimatedFrame(Base, bFlat, Tic);
		const FDoomPicture* Pic = bFlat ? Gfx->Flat(Frame) : Gfx->Texture(Frame);
		if (Pic && Pic->Texture)
		{
			Pair.Value->SetTextureParameterValue(TEXT("Tex"), Pic->Texture);
		}
	}
}

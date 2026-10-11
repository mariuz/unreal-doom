#include "DoomLevel.h"
#include "DoomMapActors.h"
#include "DoomMap.h"
#include "DoomGraphics.h"
#include "DoomAssets.h"
#include "DoomUE.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshAttributes.h"
#include "Misc/Crc.h"
#include "Async/ParallelFor.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#endif

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
	// transient: generated from the WAD every time, never saved into a level asset; unique names
	// because the editor preview rebuilds while the previous components await garbage collection
	UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(this, MakeUniqueObjectName(this, UProceduralMeshComponent::StaticClass(), FName(*Name)), RF_Transient);
	C->SetupAttachment(RootComponent);
	// cooked synchronously while the level loads, so nothing spawns before the floor exists;
	// moving chunks switch to async cooking afterwards (see Build); the editor preview never waits
	C->bUseAsyncCooking = bEditorPreview;
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
	// the map's pictures first (decoded in parallel), so generating the chunks in parallel only reads the caches
	TSharedRef<FDoomPicturePrefetch> Pictures = InGfx.CollectPrefetch(InMap);
	InGfx.DecodePrefetch(*Pictures);
	InGfx.CachePrefetch(*Pictures);
	InGfx.CreatePrefetchTextures(*Pictures, MAX_dbl);
	BeginBuild(InMap, InGfx, MovableSectors, bInModern, SkyTexture);
	TArray<FChunkSections> Sections;
	GenerateChunks(Sections);
	int32 Next = 0;
	CommitChunks(Sections, Next, MAX_dbl);
	FinishBuild();
}

void ADoomLevel::BeginBuild(FDoomMap& InMap, FDoomGraphics& InGfx, const TSet<int32>& MovableSectors, bool bInModern, FName SkyTexture)
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
}

void ADoomLevel::GenerateChunks(TArray<FChunkSections>& Out) const
{
	Out.SetNum(Chunks.Num());
	ParallelFor(Chunks.Num(), [this, &Out](int32 i)
	{
		Generate(Chunks[i], Out[i].Solid, Out[i].NonSolid);
	});
}

bool ADoomLevel::CommitChunks(TArray<FChunkSections>& Sections, int32& Next, double Deadline)
{
	while (Next < Chunks.Num())
	{
		const int32 i = Next++;
		Chunks[i].bDirty = false;
		CommitChunk(i, Sections[i]);
		Sections[i] = FChunkSections();
		if (FPlatformTime::Seconds() > Deadline)
		{
			break;
		}
	}
	return Next >= Chunks.Num();
}

void ADoomLevel::FinishBuild()
{
	BuildBlockingCollision();
	for (FChunk& C : Chunks)
	{
		if (C.bDynamic)
		{
			C.Solid->bUseAsyncCooking = true; // doors and lifts recook every tic they move
		}
	}
	UE_LOG(LogDoom, Log, TEXT("Level geometry: %d chunks, %d materials"), Chunks.Num(), Materials.Num());
}

void ADoomLevel::ClearGeometry()
{
	for (UProceduralMeshComponent* C : MeshComponents)
	{
		if (C) C->DestroyComponent();
	}
	for (UStaticMeshComponent* C : BakedComponents)
	{
		if (C) C->DestroyComponent();
	}
	MeshComponents.Reset();
	BakedComponents.Reset();
	BakedSignature = 0;
	Chunks.Reset();
	SectorChunks.Reset();
	LineChunk.Reset();
	Materials.Reset();
	Map = nullptr;
	Gfx = nullptr;
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
			MID->SetTextureParameterValue(TEXT("TexIndex"), Pic->IndexTexture);
			MID->SetTextureParameterValue(TEXT("Colormap"), Gfx->ColormapTexture());
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
			MID->SetTextureParameterValue(TEXT("TexIndex"), Pic->IndexTexture);
			MID->SetTextureParameterValue(TEXT("Colormap"), Gfx->ColormapTexture());
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
	float U0, float U1, float TexTop, float TexW, float TexH, int32 Sector, float ContrastLight, float Kind) const
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

void ADoomLevel::EmitPlanes(int32 SectorIndex, TMap<FName, FSectionData>& Out) const
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
					S.I.Append({ Base, Base + k + 1, Base + k });
				}
				else
				{
					S.I.Append({ Base, Base + k, Base + k + 1 });
				}
			}
		}
	}
}

void ADoomLevel::EmitLine(int32 LineIndex, bool bDynamic, TMap<FName, FSectionData>& OutSolid, TMap<FName, FSectionData>& OutNonSolid) const
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
			if (Tex.IsNone() || Top <= Bottom) // no zero-height quads: in collision they report bogus normals and block steps
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
				if (O.Ceiling < S.Ceiling)
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

void ADoomLevel::Generate(const FChunk& Chunk, TMap<FName, FSectionData>& OutSolid, TMap<FName, FSectionData>& OutNonSolid) const
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
	// The plugin recooks the whole component's collision on every section change, which made a
	// 136-section chunk cook 136 times (most of a map's load time). Sections are created without
	// collision while they change, then flagged together and cooked once at the end.
	bool bCollisionChanged = false;
	if (bCollision)
	{
		for (int32 i = 0; i < Comp->GetNumSections(); ++i)
		{
			Comp->GetProcMeshSection(i)->bEnableCollision = false;
		}
	}
	// sections that disappeared
	for (auto It = Sections.CreateIterator(); It; ++It)
	{
		if (!Data.Contains(It.Key()))
		{
			Comp->ClearMeshSection(It.Value());
			Counts[It.Value()] = -1;
			bCollisionChanged = true;
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
		// In-place updates only without collision: for colliding sections the plugin rewrites the
		// cooked triangle mesh's vertices directly, which breaks (out of range) as soon as any other
		// section of the component appeared or vanished while async cooking catches up.
		if (!bCollision && Existing && Counts.IsValidIndex(*Existing) && Counts[*Existing] == Signature)
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
		Comp->CreateMeshSection_LinearColor(Index, D.V, D.I, D.N, D.UV0, D.UV1, Empty, Empty, D.C, NoTangents, false, false);
		Comp->SetMaterial(Index, MaterialForKey(Pair.Key));
		Counts[Index] = Signature;
		bCollisionChanged = true;
	}
	if (bCollision)
	{
		int32 Last = INDEX_NONE;
		for (int32 i = 0; i < Comp->GetNumSections(); ++i)
		{
			FProcMeshSection* S = Comp->GetProcMeshSection(i);
			S->bEnableCollision = S->ProcIndexBuffer.Num() > 0;
			Last = S->bEnableCollision ? i : Last;
		}
		if (bCollisionChanged && Last != INDEX_NONE)
		{
			// re-setting a section is the plugin's public way to recook the component's collision
			const FProcMeshSection Copy = *Comp->GetProcMeshSection(Last);
			Comp->SetProcMeshSection(Last, Copy);
		}
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
	for (int32 i = 0; i < Chunks.Num(); ++i)
	{
		FChunk& C = Chunks[i];
		if (!C.bDirty)
		{
			continue;
		}
		C.bDirty = false;
		FChunkSections Sections;
		Generate(C, Sections.Solid, Sections.NonSolid);
		CommitChunk(i, Sections);
	}
}

void ADoomLevel::CommitChunk(int32 i, FChunkSections& Sections)
{
	FChunk& C = Chunks[i];
	TMap<FName, FSectionData>& Solid = Sections.Solid;
	TMap<FName, FSectionData>& NonSolid = Sections.NonSolid;
	Commit(C, C.Solid, C.SolidSections, C.SolidCounts, Solid, true);
	Commit(C, C.NonSolid, C.NonSolidSections, C.NonSolidCounts, NonSolid, false);
	if (i == 0 && bModern && !bEditorPreview)
	{
		// The static chunk only changes on rare texture swaps; loading a save marks it dirty
		// without changing anything, so rebake only when the geometry really differs.
		const uint32 Sig = Signature(Solid, NonSolid);
		if (Sig != BakedSignature)
		{
			int32 Verts = 0;
			for (const auto& P : Solid) Verts += P.Value.V.Num();
			for (const auto& P : NonSolid) Verts += P.Value.V.Num();
			UE_LOG(LogDoom, Log, TEXT("Static chunk: %d + %d sections, %d vertices, signature %08x"), Solid.Num(), NonSolid.Num(), Verts, Sig);
			// an imported level's saved tiles (the only way in packaged builds), else bake now
			TArray<UStaticMesh*> Saved;
			if (FindSavedTiles(Sig, Saved))
			{
				ShowTiles(Saved, true);
			}
			else
			{
				BakeStaticChunk(Solid, NonSolid);
			}
			BakedSignature = Sig;
		}
	}
}

uint32 ADoomLevel::Signature(const TMap<FName, FSectionData>& Solid, const TMap<FName, FSectionData>& NonSolid)
{
	uint32 Sig = 1;
	for (const TMap<FName, FSectionData>* Data : { &Solid, &NonSolid })
	{
		for (const auto& Pair : *Data)
		{
			const FSectionData& D = Pair.Value;
			// Content only, and stable across builds: an FName's hash depends on the process (name
			// table order) and its casing on the first use in the process, and an editor and a packaged
			// game may round the last bits of a float differently: positions (1 Unreal unit) and UVs (1/64) are
			// quantized and keys upper-cased first.
			TArray<int32> Q;
			Q.Reserve(D.V.Num() * 5 + 1);
			Q.Add(D.I.Num());
			for (int32 v = 0; v < D.V.Num(); ++v)
			{
				Q.Add(FMath::RoundToInt32(D.V[v].X));
				Q.Add(FMath::RoundToInt32(D.V[v].Y));
				Q.Add(FMath::RoundToInt32(D.V[v].Z));
				Q.Add(D.UV0.IsValidIndex(v) ? FMath::RoundToInt32(D.UV0[v].X * 64.0) : 0);
				Q.Add(D.UV0.IsValidIndex(v) ? FMath::RoundToInt32(D.UV0[v].Y * 64.0) : 0);
			}
			const uint32 H = FCrc::MemCrc32(Q.GetData(), Q.Num() * sizeof(int32));
			Sig ^= HashCombine(FCrc::StrCrc32(*Pair.Key.ToString().ToUpper()), H) + (Data == &Solid ? 0u : 0x9e3779b9u);
		}
	}
	return Sig;
}

void ADoomLevel::GetStaticSections(TMap<FName, FSectionData>& Solid, TMap<FName, FSectionData>& NonSolid)
{
	if (Chunks.Num() > 0)
	{
		Generate(Chunks[0], Solid, NonSolid);
	}
}

void ADoomLevel::GetAllSections(TMap<FName, FSectionData>& Solid, TMap<FName, FSectionData>& NonSolid)
{
	auto Merge = [](TMap<FName, FSectionData>& Into, const TMap<FName, FSectionData>& From)
	{
		for (const auto& Pair : From)
		{
			FSectionData& D = Into.FindOrAdd(Pair.Key);
			const int32 Base = D.V.Num();
			D.V.Append(Pair.Value.V);
			D.N.Append(Pair.Value.N);
			D.UV0.Append(Pair.Value.UV0);
			D.UV1.Append(Pair.Value.UV1);
			D.C.Append(Pair.Value.C);
			for (int32 I : Pair.Value.I) D.I.Add(Base + I);
		}
	};
	for (FChunk& C : Chunks)
	{
		TMap<FName, FSectionData> S, N;
		Generate(C, S, N);
		Merge(Solid, S);
		Merge(NonSolid, N);
	}
}

void ADoomLevel::SplitTiles(const TMap<FName, FSectionData>& Solid, const TMap<FName, FSectionData>& NonSolid, TArray<FTile>& Out)
{
	// one distance field per tile keeps its voxels small
	constexpr float TileSize = 1024.f * Doom::Scale;
	TMap<FIntPoint, int32> TileIndex;
	TMap<FIntPoint, TMap<FName, TMap<int32, int32>>> Remaps;
	for (const TMap<FName, FSectionData>* Data : { &Solid, &NonSolid })
	{
		for (const auto& Pair : *Data)
		{
			const FSectionData& D = Pair.Value;
			for (int32 t = 0; t + 2 < D.I.Num(); t += 3)
			{
				const FVector Mid = (D.V[D.I[t]] + D.V[D.I[t + 1]] + D.V[D.I[t + 2]]) / 3.f;
				const FIntPoint Cell(FMath::FloorToInt32(Mid.X / TileSize), FMath::FloorToInt32(Mid.Y / TileSize));
				int32* Index = TileIndex.Find(Cell);
				if (!Index)
				{
					Index = &TileIndex.Add(Cell, Out.Num());
					Out.AddDefaulted_GetRef().Cell = Cell;
				}
				FTile& Tile = Out[*Index];
				FSectionData& S = Tile.Sections.FindOrAdd(Pair.Key);
				Tile.Slots.AddUnique(Pair.Key);
				TMap<int32, int32>& Remap = Remaps.FindOrAdd(Cell).FindOrAdd(Pair.Key);
				for (int32 k = 0; k < 3; ++k)
				{
					const int32 Src = D.I[t + k];
					int32* Found = Remap.Find(Src);
					if (!Found)
					{
						Found = &Remap.Add(Src, S.V.Num());
						S.V.Add(D.V[Src]);
						S.N.Add(D.N[Src]);
						S.UV0.Add(D.UV0[Src]);
						S.UV1.Add(D.UV1[Src]);
						S.C.Add(D.C[Src]);
					}
					S.I.Add(*Found);
				}
			}
		}
	}
	// a stable order (cells sorted) so editor bakes name and save tiles the same way every time
	Out.Sort([](const FTile& A, const FTile& B) { return A.Cell.Y != B.Cell.Y ? A.Cell.Y < B.Cell.Y : A.Cell.X < B.Cell.X; });
}

#if WITH_EDITOR
void ADoomLevel::FillTileMesh(UStaticMesh* Mesh, const FTile& Tile, TFunctionRef<UMaterialInterface*(FName)> Material)
{
	FMeshDescription MD;
	FStaticMeshAttributes Attr(MD);
	Attr.Register();
	TVertexAttributesRef<FVector3f> Positions = Attr.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attr.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attr.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> Colors = Attr.GetVertexInstanceColors();
	TPolygonGroupAttributesRef<FName> SlotNames = Attr.GetPolygonGroupMaterialSlotNames();
	UVs.SetNumChannels(2);
	TArray<FStaticMaterial> StaticMaterials;
	for (const FName Key : Tile.Slots)
	{
		const FSectionData& D = Tile.Sections[Key];
		const FPolygonGroupID Group = MD.CreatePolygonGroup();
		SlotNames[Group] = Key;
		StaticMaterials.Add(FStaticMaterial(Material(Key), Key, Key));
		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(D.V.Num());
		for (int32 v = 0; v < D.V.Num(); ++v)
		{
			const FVertexID Vert = MD.CreateVertex();
			Positions[Vert] = FVector3f(D.V[v]);
			const FVertexInstanceID Inst = MD.CreateVertexInstance(Vert);
			Normals[Inst] = FVector3f(D.N[v]);
			UVs.Set(Inst, 0, FVector2f(D.UV0[v]));
			UVs.Set(Inst, 1, FVector2f(D.UV1[v]));
			// The mesh builder sRGB-encodes vertex colours while the procedural mesh stores them
			// raw, and the materials read the raw bytes (surface kind): pre-decode.
			Colors[Inst] = FVector4f(FLinearColor::FromSRGBColor(D.C[v].ToFColor(false)));
			Instances.Add(Inst);
		}
		for (int32 t = 0; t + 2 < D.I.Num(); t += 3)
		{
			MD.CreateTriangle(Group, { Instances[D.I[t]], Instances[D.I[t + 1]], Instances[D.I[t + 2]] });
		}
	}
	Mesh->SetStaticMaterials(StaticMaterials);
	FStaticMeshSourceModel& Source = Mesh->GetNumSourceModels() > 0 ? Mesh->GetSourceModel(0) : Mesh->AddSourceModel();
	Source.BuildSettings.bRecomputeNormals = false;
	Source.BuildSettings.bRecomputeTangents = true;
	Source.BuildSettings.bGenerateLightmapUVs = false;
	Source.BuildSettings.bUseFullPrecisionUVs = true; // UV1 carries the sector index
	Source.BuildSettings.bRemoveDegenerates = false;
	Source.BuildSettings.bBuildReversedIndexBuffer = false;
	Source.BuildSettings.bGenerateDistanceFieldAsIfTwoSided = true;
	Source.BuildSettings.DistanceFieldResolutionScale = 0.5f; // ~16 map units per voxel: plenty for Doom's walls
	// the distance field and card builders only take sections listed here (importers fill it)
	for (int32 Section = 0; Section < StaticMaterials.Num(); ++Section)
	{
		FMeshSectionInfo Info(Section);
		Info.bCastShadow = true;
		Info.bAffectDistanceFieldLighting = true;
		Mesh->GetSectionInfoMap().Set(0, Section, Info);
	}
	Mesh->CreateMeshDescription(0, MoveTemp(MD));
	// content-hash GUIDs make the derived data keys repeatable, so meshes and distance fields
	// come from the DDC after their first build
	UStaticMesh::FCommitMeshDescriptionParams Commit;
	Commit.bUseHashAsGuid = true;
	Mesh->CommitMeshDescription(0, Commit);
	Mesh->bGenerateMeshDistanceField = true;
}
#endif

bool ADoomLevel::FindSavedTiles(uint32 Sig, TArray<UStaticMesh*>& Out) const
{
	const ADoomMapActor* MapActor = ADoomMapActor::Find(GetWorld());
	if (!MapActor || !Map || !MapActor->MapName.Equals(Map->Name, ESearchCase::IgnoreCase) || MapActor->BakedTiles.Num() == 0)
	{
		return false;
	}
	if ((uint32)MapActor->BakedTileSignature != Sig)
	{
		UE_LOG(LogDoom, Warning, TEXT("%s: the level's baked tiles are out of date (baked %08x, now %08x): press Bake Static Meshes on its DoomMap actor"),
			*Map->Name, (uint32)MapActor->BakedTileSignature, Sig);
		return false;
	}
	for (const TSoftObjectPtr<UStaticMesh>& Tile : MapActor->BakedTiles)
	{
		UStaticMesh* Mesh = Tile.LoadSynchronous();
		if (!Mesh)
		{
			UE_LOG(LogDoom, Warning, TEXT("%s: missing baked tile %s"), *Map->Name, *Tile.ToString());
			return false;
		}
		Out.Add(Mesh);
	}
	return true;
}

void ADoomLevel::ShowTiles(const TArray<UStaticMesh*>& Meshes, bool bSaved)
{
	for (UStaticMeshComponent* C : BakedComponents)
	{
		if (C) C->DestroyComponent();
	}
	BakedComponents.Reset();
	for (UStaticMesh* Mesh : Meshes)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
		C->SetupAttachment(RootComponent);
		C->SetStaticMesh(Mesh);
		// slots are named by section key: this level's material instances (sector light, animation)
		const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			C->SetMaterial(i, MaterialForKey(Slots[i].MaterialSlotName));
		}
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(true);
		C->bAffectDistanceFieldLighting = true;
		C->RegisterComponent();
		BakedComponents.Add(C);
	}
	if (BakedComponents.Num() > 0)
	{
		// the procedural static chunk now only collides
		Chunks[0].Solid->SetVisibility(false);
		Chunks[0].NonSolid->SetVisibility(false);
	}
	bSavedTiles = bSaved;
	UE_LOG(LogDoom, Log, TEXT("The static chunk is drawn by %d %s static mesh tiles"), BakedComponents.Num(), bSaved ? TEXT("saved") : TEXT("runtime-baked"));
}

void ADoomLevel::BakeStaticChunk(const TMap<FName, FSectionData>& Solid, const TMap<FName, FSectionData>& NonSolid)
{
#if WITH_EDITOR
	TArray<FTile> Tiles;
	SplitTiles(Solid, NonSolid, Tiles);
	TArray<UStaticMesh*> Meshes;
	for (const FTile& Tile : Tiles)
	{
		UStaticMesh* Mesh = NewObject<UStaticMesh>(this, *FString::Printf(TEXT("DoomTile_%d_%d"), Tile.Cell.X, Tile.Cell.Y), RF_Transient);
		FillTileMesh(Mesh, Tile, [this](FName Key) { return MaterialForKey(Key); });
		Mesh->Build(true);
		Meshes.Add(Mesh);
	}
	FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
	ShowTiles(Meshes, false);
#endif
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
			Pair.Value->SetTextureParameterValue(TEXT("TexIndex"), Pic->IndexTexture);
		}
	}
}

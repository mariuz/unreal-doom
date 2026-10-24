// The content builder's Lyra export (-exportlyra): a map as OBJ tiles by surface role, with a JSON
// of spawns, pickups, doors and lifts for Tools/Lyra/build_doom_level.py.
#include "DoomContentBuilder.h"
#include "DoomContentPrivate.h"
#include "UObject/Linker.h"
#include "Misc/FeedbackContext.h"
#include "DoomGameData.h"
#include "DoomMapTiles.h"
#include "DoomAssets.h"
#include "DoomUE.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionSceneColor.h"
#include "Engine/Texture2D.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "NiagaraSystem.h"
#include "DoomMapActors.h"
#include "DoomMapCheck.h"
#include "DoomMapGen.h"
#include "DoomWadWriter.h"
#include "DoomAI.h"
#include "DoomWad.h"
#include "DoomSettings.h"
#include "DoomInput.h"
#include "HAL/FileManager.h"
#include "DoomMap.h"
#include "DoomGraphics.h"
#include "DoomInfo.h"
#include "DoomWorld.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "DoomLevel.h"
#include "StaticMeshCompiler.h"
#include "Engine/StaticMesh.h"
#include "DoomWad.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "NiagaraEmitterHandle.h"

// ---- Export for Lyra -----------------------------------------------------------------------------
// A WAD map as an arena for Lyra's Shooter Core: doors open, lifts lowered (Lyra launch pads take
// their place), geometry as OBJ tiles grouped by surface role so Lyra's materials can be applied,
// invisible collision ramps over Doom's steps (taller than a Lyra character's step), and a JSON
// list of gameplay spots. Tools/Lyra/build_doom_level.py builds the Lyra level from it.

namespace
{
	FString SurfaceRole(const FString& Key, const FVector3f& Normal)
	{
		const FString Name = Key.Mid(2).ToUpper();
		auto Has = [&](std::initializer_list<const TCHAR*> Parts)
		{
			for (const TCHAR* P : Parts) if (Name.Contains(P)) return true;
			return false;
		};
		if (Has({ TEXT("NUKAGE"), TEXT("FWATER"), TEXT("LAVA"), TEXT("BLOOD"), TEXT("SLIME") })) return TEXT("Liquid");
		if (Has({ TEXT("LITE"), TEXT("LIGHT"), TEXT("TLITE"), TEXT("CEIL1_2"), TEXT("CEIL1_3") })) return TEXT("Light");
		if (Has({ TEXT("DOOR"), TEXT("DOORTRAK") })) return TEXT("Door");
		if (Name.StartsWith(TEXT("SW1")) || Name.StartsWith(TEXT("SW2")) || Has({ TEXT("EXIT") })) return TEXT("Accent");
		if (Normal.Z > 0.5f) return TEXT("Floor");
		if (Normal.Z < -0.5f) return TEXT("Ceiling");
		if (Has({ TEXT("STEP"), TEXT("METAL"), TEXT("SUPPORT"), TEXT("PIPE"), TEXT("COMP") })) return TEXT("Metal");
		return TEXT("Wall");
	}

	struct FObjWriter
	{
		TMap<FString, TArray<FVector3f>> Tris; // role -> 3 positions per triangle
		TMap<FString, TArray<FVector3f>> Norms;

		// every triangle is wound so its front faces N: the level builder's walls come wound the other way
		// from its floors, which a two-sided material hides but one-sided collision does not
		void Add(const FString& Role, const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& N)
		{
			const bool bFlip = FVector3f::DotProduct(FVector3f::CrossProduct(B - A, C - A), N) > 0;
			TArray<FVector3f>& T = Tris.FindOrAdd(Role);
			T.Add(A); T.Add(bFlip ? C : B); T.Add(bFlip ? B : C);
			Norms.FindOrAdd(Role).Add(N);
		}

		bool Save(const FString& File) const
		{
			// Unreal centimetres, Z up; planar UVs: 1 UV per 4 m, floors from X/Y, walls along the wall
			FString Out = TEXT("# DoomUE export for Lyra\n");
			int32 Base = 1;
			for (const auto& Pair : Tris)
			{
				Out += FString::Printf(TEXT("g %s\nusemtl %s\n"), *Pair.Key, *Pair.Key);
				const TArray<FVector3f>& T = Pair.Value;
				const TArray<FVector3f>& NN = Norms[Pair.Key];
				for (int32 t = 0; t * 3 + 2 < T.Num(); ++t)
				{
					const FVector3f N = NN[t];
					for (int32 k = 0; k < 3; ++k)
					{
						const FVector3f& P = T[t * 3 + k];
						float U, V;
						if (FMath::Abs(N.Z) > 0.5f) { U = P.X / 400.f; V = P.Y / 400.f; }
						else { const FVector3f Tan(-N.Y, N.X, 0); U = FVector3f::DotProduct(P, Tan) / 400.f; V = -P.Z / 400.f; }
						Out += FString::Printf(TEXT("v %.2f %.2f %.2f\nvt %.4f %.4f\nvn %.4f %.4f %.4f\n"), P.X, P.Y, P.Z, U, V, N.X, N.Y, N.Z);
					}
					Out += FString::Printf(TEXT("f %d/%d/%d %d/%d/%d %d/%d/%d\n"), Base, Base, Base, Base + 1, Base + 1, Base + 1, Base + 2, Base + 2, Base + 2);
					Base += 3;
				}
			}
			return FFileHelper::SaveStringToFile(Out, *File);
		}
	};

	TSharedRef<FJsonObject> JVec(const FVector& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}
}

bool DoomContent::ExportLyraLevel(const FString& InMapName, const FString& OutDir)
{
	const FString MapName = InMapName.ToUpper();
	FDoomWad* Wad = DoomEditorData::Wad();
	FDoomGraphics* Gfx = DoomEditorData::Graphics();
	FDoomMap Map;
	if (!Wad || !Gfx || !Map.Load(*Wad, MapName))
	{
		UE_LOG(LogDoomContent, Error, TEXT("ExportLyra: no map %s"), *MapName);
		return false;
	}
	const float S = Doom::Scale;
	auto Feet = [&](const FVector2f& P) { const int32 Sec = Map.SectorAt(P); return Doom::ToUE(P, Sec >= 0 ? Map.Sectors[Sec].Floor : 0); };

	// doors open, lifts down (their old top height goes to the launch pads)
	TArray<UDoomWorld::ESectorRole> Roles;
	UDoomWorld::ClassifySectors(Map, Roles);
	TArray<float> LiftTop;
	LiftTop.Init(0, Map.Sectors.Num());
	auto Neighbours = [&](int32 Sec, TFunctionRef<void(const FDoomSector&)> Fn)
	{
		for (int32 Li : Map.Sectors[Sec].Lines)
		{
			const int32 O = Map.OtherSector(Li, Sec);
			if (O != INDEX_NONE) Fn(Map.Sectors[O]);
		}
	};
	int32 Doors = 0, Lifts = 0;
	for (int32 s = 0; s < Map.Sectors.Num(); ++s)
	{
		FDoomSector& Sec = Map.Sectors[s];
		if (Roles[s] == UDoomWorld::ESectorRole::Door)
		{
			float Lowest = TNumericLimits<float>::Max();
			Neighbours(s, [&](const FDoomSector& N) { Lowest = FMath::Min(Lowest, N.Ceiling); });
			if (Lowest < TNumericLimits<float>::Max()) Sec.Ceiling = FMath::Max(Sec.Ceiling, Lowest - 4.f);
			++Doors;
		}
		else if (Roles[s] == UDoomWorld::ESectorRole::Lift)
		{
			float Lowest = Sec.Floor;
			Neighbours(s, [&](const FDoomSector& N) { Lowest = FMath::Min(Lowest, N.Floor); });
			LiftTop[s] = Sec.Floor;
			Sec.Floor = Lowest;
			++Lifts;
		}
	}

	// geometry through the game's own builder
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false);
	ADoomLevel* Level = World->SpawnActor<ADoomLevel>();
	Level->bEditorPreview = true;
	Level->Build(Map, *Gfx, TSet<int32>(), false, FDoomGraphics::SkyTextureFor(MapName));
	TMap<FName, ADoomLevel::FSectionData> Solid, NonSolid;
	Level->GetAllSections(Solid, NonSolid);

	IFileManager::Get().MakeDirectory(*OutDir, true);
	TMap<FIntPoint, FObjWriter> Tiles;
	FBox Bounds(ForceInit);
	const float TileSize = 1024.f * S;
	int32 Triangles = 0;
	// collision-only surfaces (hidden in Lyra): step ramps, impassable lines, and Doom's sky as a shell
	FObjWriter Collision;
	for (const auto& Pair : Solid)
	{
		const FString Key = Pair.Key.ToString();
		const ADoomLevel::FSectionData& D = Pair.Value;
		if (Key.StartsWith(TEXT("S:")))
		{
			// sky: open to Lyra's sky, but as solid as Doom's (nobody jumps out of the map)
			for (int32 t = 0; t + 2 < D.I.Num(); t += 3)
			{
				Collision.Add(TEXT("Blocker"), FVector3f(D.V[D.I[t]]), FVector3f(D.V[D.I[t + 1]]), FVector3f(D.V[D.I[t + 2]]), FVector3f(D.N[D.I[t]]));
			}
			continue;
		}
		for (int32 t = 0; t + 2 < D.I.Num(); t += 3)
		{
			const FVector3f A(D.V[D.I[t]]), B(D.V[D.I[t + 1]]), C(D.V[D.I[t + 2]]);
			const FVector3f N(D.N[D.I[t]]);
			const FVector3f Mid = (A + B + C) / 3.f;
			FObjWriter& W = Tiles.FindOrAdd(FIntPoint(FMath::FloorToInt32(Mid.X / TileSize), FMath::FloorToInt32(Mid.Y / TileSize)));
			W.Add(SurfaceRole(Key, N), A, B, C, N);
			Bounds += FVector(A); Bounds += FVector(B); Bounds += FVector(C);
			++Triangles;
		}
	}
	World->DestroyWorld(false);

	TArray<TSharedPtr<FJsonValue>> TileFiles;
	for (const auto& Tile : Tiles)
	{
		const FString Name = FString::Printf(TEXT("%s_Tile_%s%d_%s%d.obj"), *MapName, Tile.Key.X < 0 ? TEXT("N") : TEXT(""), FMath::Abs(Tile.Key.X),
			Tile.Key.Y < 0 ? TEXT("N") : TEXT(""), FMath::Abs(Tile.Key.Y));
		Tile.Value.Save(OutDir / Name);
		TileFiles.Add(MakeShared<FJsonValueString>(Name));
	}

	// collision ramps over steps up to 24 map units (a Lyra character steps 45 cm; Doom's 72), and
	// walls on impassable two-sided lines (window ledges, railings), both sides, floor to ceiling
	int32 NumRamps = 0, NumBlockers = 0;
	for (const FDoomLine& L : Map.Lines)
	{
		if (L.FrontSector == INDEX_NONE || L.BackSector == INDEX_NONE)
		{
			continue;
		}
		const FDoomSector& F = Map.Sectors[L.FrontSector];
		const FDoomSector& K = Map.Sectors[L.BackSector];
		if (L.Flags & DoomLineFlags::Blocking)
		{
			const FVector2f A = Map.V(L.V1), B = Map.V(L.V2);
			const float Low = FMath::Min(F.Floor, K.Floor), High = FMath::Max(F.Ceiling, K.Ceiling);
			const FVector3f A0(Doom::ToUE(A, Low)), B0(Doom::ToUE(B, Low)), A1(Doom::ToUE(A, High)), B1(Doom::ToUE(B, High));
			const FVector2f Dir = (B - A).GetSafeNormal();
			const FVector3f Side(Doom::ToUE(FVector2f(Dir.Y, -Dir.X), 0) / Doom::Scale);
			for (const FVector3f& Facing : { Side, -Side })
			{
				Collision.Add(TEXT("Blocker"), A0, B0, B1, Facing);
				Collision.Add(TEXT("Blocker"), A0, B1, A1, Facing);
			}
			++NumBlockers;
			continue;
		}
		const float Rise = FMath::Abs(F.Floor - K.Floor);
		if (Rise < 14.f || Rise > 24.f)
		{
			continue;
		}
		const bool bFrontLow = F.Floor < K.Floor;
		const float Low = FMath::Min(F.Floor, K.Floor), High = FMath::Max(F.Floor, K.Floor);
		const FVector2f A = Map.V(L.V1), B = Map.V(L.V2);
		// the front side is on the right of V1->V2 in Doom: step out into the lower sector
		const FVector2f Dir = (B - A).GetSafeNormal();
		const FVector2f Right(Dir.Y, -Dir.X);
		const FVector2f Out = bFrontLow ? Right : -Right;
		const FVector2f A2 = A + Out * Rise * 2.2f, B2 = B + Out * Rise * 2.2f;
		const FVector3f TA(Doom::ToUE(A, High)), TB(Doom::ToUE(B, High)), BA(Doom::ToUE(A2, Low)), BB(Doom::ToUE(B2, Low));
		const FVector3f N = FVector3f::CrossProduct(TB - TA, BA - TA).GetSafeNormal();
		const FVector3f Up = N.Z < 0 ? -N : N;
		Collision.Add(TEXT("Ramp"), TA, TB, BB, Up);
		Collision.Add(TEXT("Ramp"), TA, BB, BA, Up);
		++NumRamps;
	}
	const FString RampFile = FString::Printf(TEXT("%s_Collision.obj"), *MapName);
	Collision.Save(OutDir / RampFile);

	// gameplay spots
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("map"), MapName);
	Root->SetStringField(TEXT("title"), FString::Printf(TEXT("%s"), *MapName));
	Root->SetArrayField(TEXT("tiles"), TileFiles);
	Root->SetStringField(TEXT("collision"), RampFile);
	Root->SetStringField(TEXT("ramps"), RampFile); // older builder scripts
	Root->SetObjectField(TEXT("boundsMin"), JVec(Bounds.Min));
	Root->SetObjectField(TEXT("boundsMax"), JVec(Bounds.Max));
	TArray<TSharedPtr<FJsonValue>> Starts, Items, LiftsJ, Teleports, Lights;
	auto Spot = [&](const FVector& At, float Yaw, const TCHAR* Kind)
	{
		TSharedRef<FJsonObject> O = JVec(At);
		O->SetNumberField(TEXT("yaw"), Yaw);
		O->SetStringField(TEXT("kind"), Kind);
		return MakeShared<FJsonValueObject>(O);
	};
	for (const FDoomMapThing& T : Map.Things)
	{
		const FVector At = Feet(T.P);
		const float Yaw = Doom::AngleToYaw(T.Angle);
		const FDoomThingDef* Def = DoomInfo::FindThing(T.Type);
		if (T.Type >= 1 && T.Type <= 4) { Starts.Add(Spot(At, Yaw, TEXT("coop"))); continue; }
		if (T.Type == 11) { Starts.Add(Spot(At, Yaw, TEXT("deathmatch"))); continue; }
		if (!Def) continue;
		const TCHAR* Kind = nullptr;
		switch (Def->Pickup)
		{
		case EPickup::Shotgun: case EPickup::SuperShotgun: Kind = TEXT("shotgun"); break;
		case EPickup::Chaingun: case EPickup::Plasma: case EPickup::BFG: case EPickup::RocketLauncher: Kind = TEXT("rifle"); break;
		case EPickup::Chainsaw: Kind = TEXT("pistol"); break;
		case EPickup::Health: case EPickup::Soulsphere: case EPickup::Megasphere: case EPickup::Berserk: Kind = TEXT("heal_big"); break;
		case EPickup::GreenArmor: case EPickup::BlueArmor: Kind = TEXT("heal_small"); break;
		default: break;
		}
		if (Kind)
		{
			Items.Add(Spot(At, Yaw, Kind));
		}
		if (Def->LightColor.A > 0 || ((Def->Flags & EThingFlag::FullBright) && !(Def->Flags & EThingFlag::Monster) && !(Def->Flags & EThingFlag::Pickup)))
		{
			TSharedRef<FJsonObject> O = JVec(At + FVector(0, 0, Def->Height * 0.8f * S));
			const FLinearColor C = Def->LightColor.A > 0 ? Def->LightColor : FLinearColor(0.7f, 0.8f, 1.f);
			O->SetNumberField(TEXT("r"), C.R); O->SetNumberField(TEXT("g"), C.G); O->SetNumberField(TEXT("b"), C.B);
			O->SetNumberField(TEXT("intensity"), 1.f);
			Lights.Add(MakeShared<FJsonValueObject>(O));
		}
	}
	for (int32 s = 0; s < Map.Sectors.Num(); ++s)
	{
		const FDoomSector& Sec = Map.Sectors[s];
		if (Roles[s] == UDoomWorld::ESectorRole::Lift && LiftTop[s] > Sec.Floor + 8.f)
		{
			TSharedRef<FJsonObject> O = JVec(Doom::ToUE(Sec.Center, Sec.Floor));
			O->SetNumberField(TEXT("rise"), (LiftTop[s] - Sec.Floor) * S);
			LiftsJ.Add(MakeShared<FJsonValueObject>(O));
		}
		// bright rooms get a soft light near the ceiling (Lyra's lighting does the rest)
		const FVector2f Size = Sec.Bounds.GetSize();
		if (Sec.Light >= 192 && !FDoomGraphics::IsSkyFlat(Sec.CeilingPic) && Size.X * Size.Y > 64 * 64 && Sec.Ceiling - Sec.Floor > 40.f)
		{
			TSharedRef<FJsonObject> O = JVec(Doom::ToUE(Sec.Center, Sec.Ceiling - 16.f));
			O->SetNumberField(TEXT("r"), 1.f); O->SetNumberField(TEXT("g"), 0.95f); O->SetNumberField(TEXT("b"), 0.9f);
			O->SetNumberField(TEXT("intensity"), Sec.Light / 255.f);
			O->SetNumberField(TEXT("radius"), FMath::Max(Size.X, Size.Y) * S);
			Lights.Add(MakeShared<FJsonValueObject>(O));
		}
	}
	TArray<TPair<int32, int32>> Links;
	UDoomWorld::FindTeleporters(Map, Links);
	for (const TPair<int32, int32>& Link : Links)
	{
		const FDoomLine& L = Map.Lines[Link.Key];
		const FVector2f A = Map.V(L.V1), B = Map.V(L.V2);
		const FVector2f Mid = (A + B) * 0.5f;
		const FDoomMapThing& Dest = Map.Things[Link.Value];
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("from"), JVec(Feet(Mid)));
		O->SetNumberField(TEXT("fromYaw"), FMath::RadiansToDegrees(FMath::Atan2(-(B - A).Y, (B - A).X)));
		O->SetObjectField(TEXT("to"), JVec(Feet(Dest.P)));
		O->SetNumberField(TEXT("toYaw"), Doom::AngleToYaw(Dest.Angle));
		Teleports.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("playerStarts"), Starts);
	Root->SetArrayField(TEXT("items"), Items);
	Root->SetArrayField(TEXT("lifts"), LiftsJ);
	Root->SetArrayField(TEXT("teleporters"), Teleports);
	Root->SetArrayField(TEXT("lights"), Lights);
	FString Json;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	const bool bOk = FFileHelper::SaveStringToFile(Json, *(OutDir / FString::Printf(TEXT("%s.json"), *MapName)));
	UE_LOG(LogDoomContent, Display, TEXT("ExportLyra: %s -> %s: %d tiles, %d triangles, %d ramps, %d impassable lines walled, %d doors opened, %d lifts, %d starts, %d items, %d teleporters, %d lights"),
		*MapName, *OutDir, Tiles.Num(), Triangles, NumRamps, NumBlockers, Doors, Lifts, Starts.Num(), Items.Num(), Teleports.Num(), Lights.Num());
	return bOk;
}

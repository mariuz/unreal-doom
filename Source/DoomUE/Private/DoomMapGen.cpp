// The random map generator (see DoomMapGen.h): rooms and corridors painted on a tile grid, the
// lines traced from the tile edges, then the node builder.
#include "DoomMapGen.h"
#include "DoomMap.h"
#include "DoomGraphics.h"
#include "DoomUE.h"

namespace
{
	constexpr int32 TileSize = 32;
	constexpr int32 CellTiles = 20;  // a room's cell: the room plus at least 2 tiles of gap each side
	constexpr int32 MaxStep = 24;    // Doom's highest step up

	enum class EKind : uint8 { Room, Corridor, Door, Alcove, Pool, Platform };

	struct FGenSector
	{
		EKind Kind = EKind::Room;
		float Floor = 0, Ceiling = 128;
		FName FloorPic, CeilingPic, Wall, Step;
		FName DoorTex, Track;  // doors
		int16 Light = 160, Special = 0;
		int16 DoorSpecial = 0; // doors: the line special on both faces
		int32 Lock = 0;        // doors: 0 none, 1 blue, 2 yellow, 3 red
		int32 Room = INDEX_NONE;
	};

	struct FRoom
	{
		int32 CX = 0, CY = 0;
		int32 X0 = 0, Y0 = 0, X1 = 0, Y1 = 0; // tiles, end exclusive
		int32 Sector = INDEX_NONE;
		int32 Platform = INDEX_NONE;          // a raised (or sunken: a pool) sector inside
		int32 Parent = INDEX_NONE, ParentLink = INDEX_NONE;
		int32 Depth = 0, Zone = 0;
		float Floor = 0;
		bool bOutdoor = false, bMainPath = false;
	};

	struct FLink
	{
		int32 A = 0, B = 0;   // rooms; A is nearer the start on tree links, and has the door
		bool bTree = true, bDoor = false, bUsed = true;
		int32 Lock = 0;
		int32 Width = 2;
		bool bAlongX = true;  // the corridor leaves A along X (else along Y)
		TArray<FIntPoint> Path;       // the brush's corner, one tile per step
		TArray<FIntPoint> DoorTiles;  // the slice next to A
		FIntPoint DoorEnds[2];        // the tiles beyond the slice's ends (must stay void: the tracks)
		int32 DoorSector = INDEX_NONE;
		bool bZ = false;              // not straight: gentler stairs
	};

	const TCHAR* const Walls[] = { TEXT("STARTAN3"), TEXT("STARTAN2"), TEXT("STARG3"), TEXT("STARGR1"), TEXT("BROWN1"), TEXT("BROWN96"),
		TEXT("BROWNGRN"), TEXT("COMPSPAN"), TEXT("GRAY1"), TEXT("GRAY4"), TEXT("METAL1"), TEXT("ICKWALL3"), TEXT("TEKWALL4"), TEXT("BRONZE1"),
		TEXT("STONE2"), TEXT("STONE3"), TEXT("CEMENT7"), TEXT("BRICK7"), TEXT("SILVER1"), TEXT("MARBLE1"), TEXT("SLADWALL"), TEXT("GSTONE1"),
		TEXT("TEKGREN1"), TEXT("PIPE4"), TEXT("SHAWN2") };
	const TCHAR* const Floors[] = { TEXT("FLOOR4_8"), TEXT("FLOOR5_1"), TEXT("FLOOR5_2"), TEXT("FLOOR0_1"), TEXT("FLOOR0_3"), TEXT("FLOOR7_1"),
		TEXT("FLAT5_4"), TEXT("FLAT14"), TEXT("FLAT1"), TEXT("FLAT5"), TEXT("FLOOR1_1"), TEXT("FLOOR3_3"), TEXT("FLAT1_1"), TEXT("MFLR8_1") };
	const TCHAR* const Ceilings[] = { TEXT("CEIL3_5"), TEXT("CEIL5_1"), TEXT("CEIL3_3"), TEXT("TLITE6_4"), TEXT("TLITE6_1"), TEXT("FLAT2"),
		TEXT("CEIL1_1"), TEXT("CEIL3_4"), TEXT("FLAT18") };
	const TCHAR* const OutdoorFloors[] = { TEXT("GRASS1"), TEXT("MFLR8_4"), TEXT("RROCK19"), TEXT("FLAT10"), TEXT("FLOOR6_2"), TEXT("GRNROCK"), TEXT("MFLR8_3") };
	const TCHAR* const Steps[] = { TEXT("STEP6"), TEXT("STEP1"), TEXT("STEP2"), TEXT("STEP4"), TEXT("STEP5") };
	const TCHAR* const SmallDoors[] = { TEXT("DOOR3"), TEXT("DOOR1") };
	const TCHAR* const BigDoors[] = { TEXT("BIGDOOR2"), TEXT("BIGDOOR1"), TEXT("BIGDOOR4") };
	const TCHAR* const ExitSwitches[] = { TEXT("SW1EXIT"), TEXT("SW1STRTN"), TEXT("SW1COMP"), TEXT("SW1BRN1"), TEXT("SW1GRAY") };

	struct FGen
	{
		const DoomMapGen::FOptions& Opt;
		const FDoomGraphics* Gfx;
		FRandomStream Rng;
		int32 G = 4;          // cells per side
		int32 W = 0, H = 0;   // tiles
		TArray<int32> Tiles;  // sector, or INDEX_NONE for void
		TArray<int32> TileLink, TileStep; // corridor tiles before they get a sector
		TArray<FGenSector> Sectors;
		TArray<FRoom> Rooms;
		TArray<FLink> Links;
		TArray<FDoomMapThing> Things;
		TArray<uint8> Occupied;
		int32 StartRoom = 0, ExitRoom = 0, MaxDepth = 1;
		int32 Alcove = INDEX_NONE;
		bool bAlcoveAlongX = true; // the exit switch is a wall along X
		TArray<int32> KeyRooms;    // per lock (1..3), the room holding its key
		FName CorridorWall, CorridorFloor, CorridorCeiling, StepTex, Track;
		DoomMapGen::FReport Report;

		FGen(const DoomMapGen::FOptions& InOpt, const FDoomGraphics* InGfx, int32 Seed) : Opt(InOpt), Gfx(InGfx), Rng(Seed) {}

		bool Exists(FName N, bool bFlat) const
		{
			return !Gfx || (bFlat ? Gfx->FlatNames().Contains(N) : Gfx->HasTexture(N));
		}
		template <int32 Num>
		FName Pick(const TCHAR* const (&Names)[Num], bool bFlat, FName Fallback = NAME_None)
		{
			TArray<FName> Ok;
			for (const TCHAR* N : Names)
			{
				if (Exists(FName(N), bFlat)) Ok.Add(FName(N));
			}
			if (Ok.Num() == 0) return Fallback.IsNone() ? FName(Names[0]) : Fallback;
			return Ok[Rng.RandRange(0, Ok.Num() - 1)];
		}
		template <int32 Num>
		FName First(const TCHAR* const (&Names)[Num], bool bFlat) const
		{
			for (const TCHAR* N : Names)
			{
				if (Exists(FName(N), bFlat)) return FName(N);
			}
			return FName(Names[0]);
		}

		int32 Idx(int32 X, int32 Y) const { return Y * W + X; }
		bool In(int32 X, int32 Y) const { return X >= 0 && Y >= 0 && X < W && Y < H; }
		int32 At(int32 X, int32 Y) const { return In(X, Y) ? Tiles[Idx(X, Y)] : INDEX_NONE; }
		bool Free(int32 X, int32 Y) const { return In(X, Y) && Tiles[Idx(X, Y)] == INDEX_NONE && TileLink[Idx(X, Y)] == INDEX_NONE; }

		int32 AddSector(const FGenSector& S) { return Sectors.Add(S); }

		// ---- layout ----

		void Layout()
		{
			const int32 N = FMath::Clamp(Opt.Rooms, 4, 24);
			G = FMath::Max(3, FMath::CeilToInt(FMath::Sqrt(N * 1.8f)));
			W = H = G * CellTiles + 2;
			Tiles.Init(INDEX_NONE, W * H);
			TileLink.Init(INDEX_NONE, W * H);
			TileStep.Init(0, W * H);
			Occupied.Init(0, W * H);
			TArray<int32> CellRoom;
			CellRoom.Init(INDEX_NONE, G * G);
			auto AddRoom = [&](int32 CX, int32 CY, int32 Parent)
			{
				FRoom R;
				R.CX = CX; R.CY = CY;
				const int32 RW = Rng.RandRange(8, 15), RH = Rng.RandRange(8, 15);
				R.X0 = 1 + CX * CellTiles + 2 + Rng.RandRange(0, CellTiles - 4 - RW);
				R.Y0 = 1 + CY * CellTiles + 2 + Rng.RandRange(0, CellTiles - 4 - RH);
				R.X1 = R.X0 + RW;
				R.Y1 = R.Y0 + RH;
				R.Parent = Parent;
				const int32 Index = Rooms.Add(R);
				CellRoom[CY * G + CX] = Index;
				if (Parent != INDEX_NONE)
				{
					FLink L;
					L.A = Parent; L.B = Index;
					Rooms[Index].ParentLink = Links.Add(L);
					Rooms[Index].Depth = Rooms[Parent].Depth + 1;
				}
			};
			AddRoom(Rng.RandRange(0, G - 1), Rng.RandRange(0, G - 1), INDEX_NONE);
			static const FIntPoint Dirs[4] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
			for (int32 Guard = 0; Rooms.Num() < N && Guard < 20000; ++Guard)
			{
				// grow from the newer rooms more often: longer paths to the exit
				const int32 From = Rng.FRand() < 0.6f ? Rng.RandRange(FMath::Max(0, Rooms.Num() - 3), Rooms.Num() - 1) : Rng.RandRange(0, Rooms.Num() - 1);
				const FIntPoint D = Dirs[Rng.RandRange(0, 3)];
				const int32 CX = Rooms[From].CX + D.X, CY = Rooms[From].CY + D.Y;
				if (CX < 0 || CY < 0 || CX >= G || CY >= G || CellRoom[CY * G + CX] != INDEX_NONE) continue;
				AddRoom(CX, CY, From);
			}
			StartRoom = 0;
			ExitRoom = 0;
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				if (Rooms[i].Depth > Rooms[ExitRoom].Depth) ExitRoom = i;
			}
			MaxDepth = FMath::Max(1, Rooms[ExitRoom].Depth);
			TArray<int32> MainPath;
			for (int32 R = ExitRoom; R != INDEX_NONE; R = Rooms[R].Parent)
			{
				Rooms[R].bMainPath = true;
				MainPath.Insert(R, 0);
			}

			// keyed doors along the way to the exit, each key somewhere before its door
			const int32 PathLinks = MainPath.Num() - 1;
			const int32 NumKeys = Rooms.Num() >= 5 ? FMath::Clamp((PathLinks - 1) / 2, 0, 3) : 0;
			TArray<int32> Colors = { 1, 2, 3 };
			for (int32 i = 2; i > 0; --i) Colors.Swap(i, Rng.RandRange(0, i));
			int32 LastAt = 0;
			for (int32 k = 0; k < NumKeys; ++k)
			{
				const int32 At = FMath::Clamp(FMath::RoundToInt((k + 1) * PathLinks / float(NumKeys + 1)) + 1, LastAt + 1, PathLinks);
				LastAt = At;
				FLink& L = Links[Rooms[MainPath[At]].ParentLink];
				L.Lock = Colors[k];
				L.bDoor = true;
			}
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				FRoom& R = Rooms[i];
				if (R.Parent != INDEX_NONE)
				{
					R.Zone = Rooms[R.Parent].Zone + (Links[R.ParentLink].Lock ? 1 : 0);
				}
			}
			KeyRooms.Init(INDEX_NONE, 4);
			for (int32 k = 0; k < NumKeys; ++k)
			{
				// the deepest side room of the zone before the door, else the room just before it
				int32 Best = INDEX_NONE;
				for (int32 i = 0; i < Rooms.Num(); ++i)
				{
					if (Rooms[i].Zone != k || KeyRooms.Contains(i)) continue;
					const bool bSide = !Rooms[i].bMainPath;
					if (Best == INDEX_NONE || (bSide && Rooms[Best].bMainPath) || (bSide == !Rooms[Best].bMainPath && Rooms[i].Depth > Rooms[Best].Depth))
					{
						Best = i;
					}
				}
				KeyRooms[Colors[k]] = Best != INDEX_NONE ? Best : StartRoom;
			}
			Report.Keys = NumKeys;

			// a few loops, inside one zone so they never get round a keyed door
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				for (int32 j = i + 1; j < Rooms.Num(); ++j)
				{
					const FRoom& A = Rooms[i];
					const FRoom& B = Rooms[j];
					if (FMath::Abs(A.CX - B.CX) + FMath::Abs(A.CY - B.CY) != 1 || A.Zone != B.Zone) continue;
					if (A.Parent == j || B.Parent == i || Rng.FRand() > 0.3f) continue;
					FLink L;
					L.A = i; L.B = j; L.bTree = false;
					Links.Add(L);
				}
			}
			for (FLink& L : Links)
			{
				if (!L.Lock) L.bDoor = Rng.FRand() < (L.bTree ? 0.45f : 0.3f);
			}
		}

		/** The corridor's brush path from A's wall to B's. */
		void MakePath(FLink& L)
		{
			const FRoom& A = Rooms[L.A];
			const FRoom& B = Rooms[L.B];
			L.bAlongX = A.CY == B.CY;
			// (u, v): u along the corridor's axis, v across it
			auto U0 = [&](const FRoom& R) { return L.bAlongX ? R.X0 : R.Y0; };
			auto U1 = [&](const FRoom& R) { return L.bAlongX ? R.X1 : R.Y1; };
			auto V0 = [&](const FRoom& R) { return L.bAlongX ? R.Y0 : R.X0; };
			auto V1 = [&](const FRoom& R) { return L.bAlongX ? R.Y1 : R.X1; };
			const int32 Dir = U0(B) > U0(A) ? 1 : -1;
			auto Overlap = [&](int32 Wd, int32& Lo, int32& Hi)
			{
				Lo = FMath::Max(V0(A), V0(B)) + 1;
				Hi = FMath::Min(V1(A), V1(B)) - 1 - Wd;
				return Lo <= Hi;
			};
			int32 Lo, Hi;
			L.Width = (Rng.FRand() < 0.3f && Overlap(4, Lo, Hi)) ? 4 : 2;
			const int32 Wd = L.Width;
			const int32 StartU = Dir > 0 ? U1(A) : U0(A) - Wd;
			const int32 EndU = Dir > 0 ? U0(B) - Wd : U1(B);
			TArray<FIntPoint> UV;
			int32 VA;
			if (Overlap(Wd, Lo, Hi))
			{
				VA = Rng.RandRange(Lo, Hi);
				for (int32 U = StartU; ; U += Dir)
				{
					UV.Add({ U, VA });
					if (U == EndU) break;
				}
			}
			else
			{
				L.bZ = true;
				VA = Rng.RandRange(V0(A) + 1, V1(A) - 1 - Wd);
				const int32 VB = Rng.RandRange(V0(B) + 1, V1(B) - 1 - Wd);
				// the turn is past the door's slice, so the door keeps solid walls beside it
				const int32 MidU = Dir > 0 ? Rng.RandRange(StartU + 1, EndU) : Rng.RandRange(EndU, StartU - 1);
				for (int32 U = StartU; ; U += Dir)
				{
					UV.Add({ U, VA });
					if (U == MidU) break;
				}
				const int32 VDir = VB > VA ? 1 : -1;
				for (int32 V = VA + VDir; VB != VA; V += VDir)
				{
					UV.Add({ MidU, V });
					if (V == VB) break;
				}
				for (int32 U = MidU + Dir; MidU != EndU; U += Dir)
				{
					UV.Add({ U, VB });
					if (U == EndU) break;
				}
			}
			auto ToXY = [&](int32 U, int32 V) { return L.bAlongX ? FIntPoint(U, V) : FIntPoint(V, U); };
			L.Path.Reset();
			for (const FIntPoint& P : UV) L.Path.Add(ToXY(P.X, P.Y));
			const int32 DoorU = Dir > 0 ? StartU : StartU + Wd - 1;
			L.DoorTiles.Reset();
			for (int32 k = 0; k < Wd; ++k) L.DoorTiles.Add(ToXY(DoorU, VA + k));
			L.DoorEnds[0] = ToXY(DoorU, VA - 1);
			L.DoorEnds[1] = ToXY(DoorU, VA + Wd);
		}

		/** The highest floor difference a corridor of this path can climb with steps of 24. */
		int32 Climb(const FLink& L) const
		{
			return (L.bZ ? 8 : 12) * (L.Path.Num() + 1);
		}

		float CorridorHeight(const FLink& L, int32 Step) const
		{
			const float FA = Rooms[L.A].Floor, FB = Rooms[L.B].Floor;
			const float Hgt = FA + (FB - FA) * (Step + 1) / float(L.Path.Num() + 1);
			return FMath::RoundToFloat(Hgt / 8.f) * 8.f;
		}

		void Heights()
		{
			static const int32 Deltas[] = { -48, -32, -16, 0, 0, 0, 16, 24, 32, 48, 64 };
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				FRoom& R = Rooms[i];
				if (R.Parent == INDEX_NONE)
				{
					R.Floor = 0;
					continue;
				}
				const int32 Limit = Climb(Links[R.ParentLink]);
				const int32 D = FMath::Clamp(Deltas[Rng.RandRange(0, UE_ARRAY_COUNT(Deltas) - 1)], -Limit, Limit);
				R.Floor = FMath::Clamp(Rooms[R.Parent].Floor + (D / 8) * 8, -256.f, 384.f);
			}
			for (FLink& L : Links)
			{
				if (!L.bTree && FMath::Abs(Rooms[L.A].Floor - Rooms[L.B].Floor) > Climb(L)) L.bUsed = false;
			}
		}

		void PaintRooms()
		{
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				FRoom& R = Rooms[i];
				R.bOutdoor = i != StartRoom && Rng.FRand() < 0.2f;
				FGenSector S;
				S.Kind = EKind::Room;
				S.Room = i;
				S.Floor = R.Floor;
				S.Wall = Pick(Walls, false);
				S.Step = StepTex;
				S.FloorPic = R.bOutdoor ? Pick(OutdoorFloors, true, FName(TEXT("FLOOR4_8"))) : Pick(Floors, true);
				if (R.bOutdoor)
				{
					S.Ceiling = R.Floor + 256;
					S.CeilingPic = FName(TEXT("F_SKY1"));
					S.Light = 208;
				}
				else
				{
					static const int32 CeilHeights[] = { 128, 144, 160, 192, 224 };
					S.Ceiling = R.Floor + CeilHeights[Rng.RandRange(0, UE_ARRAY_COUNT(CeilHeights) - 1)];
					S.CeilingPic = Pick(Ceilings, true);
					static const int16 Lights[] = { 144, 160, 176, 192, 208, 224 };
					S.Light = Lights[Rng.RandRange(0, UE_ARRAY_COUNT(Lights) - 1)];
					const float F = Rng.FRand();
					S.Special = i == StartRoom ? 0 : F < 0.06f ? 1 : F < 0.1f ? 8 : 0; // blinking, glowing
				}
				R.Sector = AddSector(S);
				for (int32 Y = R.Y0; Y < R.Y1; ++Y)
				{
					for (int32 X = R.X0; X < R.X1; ++X) Tiles[Idx(X, Y)] = R.Sector;
				}
			}
		}

		void PaintCorridors()
		{
			// doors first, so no corridor runs over another link's door slice
			for (int32 l = 0; l < Links.Num(); ++l)
			{
				FLink& L = Links[l];
				if (!L.bUsed || !L.bDoor) continue;
				FGenSector S;
				S.Kind = EKind::Door;
				const FGenSector& RoomS = Sectors[Rooms[L.A].Sector];
				S.Floor = S.Ceiling = Rooms[L.A].Floor;
				S.FloorPic = CorridorFloor;
				S.CeilingPic = Exists(FName(TEXT("FLAT20")), true) ? FName(TEXT("FLAT20")) : CorridorCeiling;
				S.Wall = CorridorWall;
				S.Step = StepTex;
				S.Light = FMath::Min<int16>(RoomS.Light, 176);
				S.DoorTex = L.Width >= 4 ? Pick(BigDoors, false) : Pick(SmallDoors, false);
				S.Lock = L.Lock;
				static const TCHAR* const LockTracks[] = { TEXT("DOORTRAK"), TEXT("DOORBLU"), TEXT("DOORYEL"), TEXT("DOORRED") };
				S.Track = L.Lock && Exists(FName(LockTracks[L.Lock]), false) ? FName(LockTracks[L.Lock]) : Track;
				static const int16 LockSpecials[] = { 1, 26, 27, 28 }; // DR door, DR blue / yellow / red
				S.DoorSpecial = LockSpecials[L.Lock];
				L.DoorSector = AddSector(S);
				for (const FIntPoint& T : L.DoorTiles)
				{
					if (Free(T.X, T.Y)) Tiles[Idx(T.X, T.Y)] = L.DoorSector;
				}
			}
			for (int32 l = 0; l < Links.Num(); ++l)
			{
				const FLink& L = Links[l];
				if (!L.bUsed) continue;
				for (int32 s = 0; s < L.Path.Num(); ++s)
				{
					for (int32 j = 0; j < L.Width; ++j)
					{
						for (int32 i = 0; i < L.Width; ++i)
						{
							const int32 X = L.Path[s].X + i, Y = L.Path[s].Y + j;
							if (!Free(X, Y)) continue;
							TileLink[Idx(X, Y)] = l;
							TileStep[Idx(X, Y)] = s;
						}
					}
				}
			}
			// a sector for each stair step of each corridor
			TMap<TPair<int32, int32>, int32> StepSectors;
			for (int32 k = 0; k < Tiles.Num(); ++k)
			{
				const int32 l = TileLink[k];
				if (l == INDEX_NONE) continue;
				const float Hgt = CorridorHeight(Links[l], TileStep[k]);
				int32& S = StepSectors.FindOrAdd({ l, (int32)Hgt }, INDEX_NONE);
				if (S == INDEX_NONE)
				{
					FGenSector C;
					C.Kind = EKind::Corridor;
					C.Floor = Hgt;
					C.Ceiling = Hgt + 112;
					C.FloorPic = CorridorFloor;
					C.CeilingPic = CorridorCeiling;
					C.Wall = CorridorWall;
					C.Step = StepTex;
					C.Light = 144;
					S = AddSector(C);
				}
				Tiles[k] = S;
			}
		}

		/** Pillars, a raised platform or a nukage pool in the middle of some rooms. */
		void PaintFeatures()
		{
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				FRoom& R = Rooms[i];
				const int32 IX0 = R.X0 + 3, IY0 = R.Y0 + 3, IX1 = R.X1 - 3, IY1 = R.Y1 - 3;
				if (IX1 - IX0 < 3 || IY1 - IY0 < 3) continue;
				const float F = Rng.FRand();
				const FGenSector& RoomS = Sectors[R.Sector];
				if (F < 0.4f && i != StartRoom)
				{
					FGenSector S = RoomS;
					const bool bPool = F < 0.18f;
					S.Kind = bPool ? EKind::Pool : EKind::Platform;
					S.Floor = RoomS.Floor + (bPool ? -16 : 16);
					if (bPool)
					{
						S.FloorPic = First({ TEXT("NUKAGE1"), TEXT("FWATER1"), TEXT("BLOOD1") }, true);
						S.Special = 7; // 5% damage
					}
					else
					{
						S.FloorPic = Pick(Floors, true);
						S.Light = FMath::Min<int16>(RoomS.Light + 16, 255);
					}
					R.Platform = AddSector(S);
					for (int32 Y = IY0; Y < IY1; ++Y)
					{
						for (int32 X = IX0; X < IX1; ++X) Tiles[Idx(X, Y)] = R.Platform;
					}
				}
				else if (F < 0.7f && R.X1 - R.X0 >= 11 && R.Y1 - R.Y0 >= 11)
				{
					const FIntPoint Corners[4] = { {IX0, IY0}, {IX1 - 2, IY0}, {IX0, IY1 - 2}, {IX1 - 2, IY1 - 2} };
					for (const FIntPoint& C : Corners)
					{
						for (int32 Y = C.Y; Y < C.Y + 2; ++Y)
						{
							for (int32 X = C.X; X < C.X + 2; ++X) Tiles[Idx(X, Y)] = INDEX_NONE;
						}
					}
				}
			}
		}

		/** A 2-tile alcove off the exit room with the exit switch at its back. */
		bool PaintExit()
		{
			const FRoom& R = Rooms[ExitRoom];
			struct FTry { FIntPoint A, B; bool bAlongX; FIntPoint Out; };
			TArray<FTry> Tries;
			// along each wall, centre first
			for (int32 Off = 0; Off < 8; ++Off)
			{
				for (int32 Sgn = -1; Sgn <= 1; Sgn += 2)
				{
					const int32 X = (R.X0 + R.X1) / 2 - 1 + Sgn * Off, Y = (R.Y0 + R.Y1) / 2 - 1 + Sgn * Off;
					if (X > R.X0 && X + 2 < R.X1)
					{
						Tries.Add({ {X, R.Y1}, {X + 1, R.Y1}, true, {0, 1} });
						Tries.Add({ {X, R.Y0 - 1}, {X + 1, R.Y0 - 1}, true, {0, -1} });
					}
					if (Y > R.Y0 && Y + 2 < R.Y1)
					{
						Tries.Add({ {R.X1, Y}, {R.X1, Y + 1}, false, {1, 0} });
						Tries.Add({ {R.X0 - 1, Y}, {R.X0 - 1, Y + 1}, false, {-1, 0} });
					}
				}
			}
			for (const FTry& T : Tries)
			{
				bool bOk = Free(T.A.X, T.A.Y) && Free(T.B.X, T.B.Y);
				// nothing but void around it (and the room behind it)
				for (const FIntPoint& P : { T.A, T.B })
				{
					static const FIntPoint N4[4] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
					for (const FIntPoint& D : N4)
					{
						const FIntPoint Q = P + D;
						if (Q == T.A || Q == T.B) continue;
						if (Q == P - T.Out)
						{
							bOk &= At(Q.X, Q.Y) == R.Sector;
						}
						else
						{
							bOk &= Free(Q.X, Q.Y);
						}
					}
				}
				if (!bOk) continue;
				FGenSector S = Sectors[R.Sector];
				S.Kind = EKind::Alcove;
				S.Ceiling = S.Floor + 72;
				S.CeilingPic = First({ TEXT("TLITE6_4"), TEXT("CEIL1_1"), TEXT("FLAT2") }, true);
				S.Light = 224;
				S.Special = 0;
				S.Wall = First({ TEXT("EXITSTON"), TEXT("STARTAN3"), TEXT("BROWN1") }, false);
				Alcove = AddSector(S);
				Tiles[Idx(T.A.X, T.A.Y)] = Tiles[Idx(T.B.X, T.B.Y)] = Alcove;
				bAlcoveAlongX = T.bAlongX;
				return true;
			}
			return false;
		}

		/** Doors with solid walls beside them (the tracks), between their room and their corridor. */
		bool DoorsSound() const
		{
			for (const FLink& L : Links)
			{
				if (!L.bUsed || !L.bDoor) continue;
				for (const FIntPoint& T : L.DoorTiles)
				{
					if (At(T.X, T.Y) != L.DoorSector) return false;
				}
				for (const FIntPoint& E : L.DoorEnds)
				{
					if (At(E.X, E.Y) != INDEX_NONE) return false;
				}
			}
			return true;
		}

		// ---- things ----

		FVector2f TileCenter(int32 X, int32 Y) const
		{
			return FVector2f((X - W / 2) * TileSize + TileSize / 2, (Y - H / 2) * TileSize + TileSize / 2);
		}

		/** A free tile of the room (its floor, or its platform / pool) with free tiles all round. */
		bool Spot(int32 RoomIndex, bool bPlatformOk, FIntPoint& Out, bool bCentre = false)
		{
			const FRoom& R = Rooms[RoomIndex];
			TArray<FIntPoint> Cands;
			for (int32 Y = R.Y0 + 1; Y < R.Y1 - 1; ++Y)
			{
				for (int32 X = R.X0 + 1; X < R.X1 - 1; ++X)
				{
					bool bOk = true;
					for (int32 dy = -1; dy <= 1 && bOk; ++dy)
					{
						for (int32 dx = -1; dx <= 1 && bOk; ++dx)
						{
							const int32 S = At(X + dx, Y + dy);
							bOk = (S == R.Sector || (bPlatformOk && S == R.Platform && S != INDEX_NONE)) && !Occupied[Idx(X + dx, Y + dy)];
						}
					}
					if (bOk) Cands.Add({ X, Y });
				}
			}
			if (Cands.Num() == 0) return false;
			if (bCentre)
			{
				const FVector2f C((R.X0 + R.X1) * 0.5f, (R.Y0 + R.Y1) * 0.5f);
				Cands.Sort([&](const FIntPoint& A, const FIntPoint& B) { return FVector2f::DistSquared(FVector2f(A), C) < FVector2f::DistSquared(FVector2f(B), C); });
				Out = Cands[0];
			}
			else
			{
				Out = Cands[Rng.RandRange(0, Cands.Num() - 1)];
			}
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				for (int32 dx = -1; dx <= 1; ++dx) Occupied[Idx(Out.X + dx, Out.Y + dy)] = 1;
			}
			return true;
		}

		bool Place(int32 RoomIndex, int32 Type, uint16 Flags, bool bPlatformOk = true, float Angle = -1, bool bCentre = false)
		{
			FIntPoint T;
			if (!Spot(RoomIndex, bPlatformOk, T, bCentre)) return false;
			FDoomMapThing Th;
			Th.P = TileCenter(T.X, T.Y);
			Th.Type = Type;
			Th.Flags = Flags;
			Th.Angle = Angle >= 0 ? Angle : 45.f * Rng.RandRange(0, 7);
			Things.Add(Th);
			return true;
		}

		int32 Monster(float F)
		{
			struct FW { int32 Type; int32 Weight; float From, To; bool bDoom2; };
			static const FW Table[] = {
				{ 3004, 4, 0.0f, 0.5f, false }, { 9, 3, 0.0f, 0.8f, false }, { 3001, 4, 0.0f, 1.1f, false },
				{ 3002, 3, 0.3f, 1.1f, false }, { 58, 2, 0.4f, 1.1f, false }, { 3006, 1, 0.4f, 1.1f, false },
				{ 3005, 2, 0.5f, 1.1f, false }, { 3003, 1, 0.8f, 1.1f, false },
				{ 65, 2, 0.3f, 1.1f, true }, { 69, 1, 0.6f, 1.1f, true }, { 66, 1, 0.75f, 1.1f, true } };
			int32 Total = 0;
			for (const FW& E : Table)
			{
				if (F >= E.From && F < E.To && (!E.bDoom2 || Opt.bDoom2)) Total += E.Weight;
			}
			int32 Roll = Rng.RandRange(0, FMath::Max(0, Total - 1));
			for (const FW& E : Table)
			{
				if (F >= E.From && F < E.To && (!E.bDoom2 || Opt.bDoom2))
				{
					if (Roll < E.Weight) return E.Type;
					Roll -= E.Weight;
				}
			}
			return 3004;
		}

		void PlaceThings()
		{
			const FRoom& S = Rooms[StartRoom];
			// face the first corridor
			float Facing = 0;
			for (const FLink& L : Links)
			{
				if (L.bUsed && (L.A == StartRoom || L.B == StartRoom))
				{
					const FRoom& O = Rooms[L.A == StartRoom ? L.B : L.A];
					Facing = O.CX > S.CX ? 0.f : O.CX < S.CX ? 180.f : O.CY > S.CY ? 90.f : 270.f;
					break;
				}
			}
			Place(StartRoom, 1, 7, false, Facing, true);
			for (int32 p = 2; p <= 4; ++p) Place(StartRoom, p, 7, false, Facing);
			Place(StartRoom, 2001, 7, false); // shotgun
			int32 Dm = 0;
			for (int32 i = 0; i < Rooms.Num() && Dm < 10; ++i)
			{
				Dm += Place(i, 11, 7, false) ? 1 : 0;
			}
			static const int32 KeyTypes[] = { 0, 5, 6, 13 };
			for (int32 c = 1; c <= 3; ++c)
			{
				if (KeyRooms[c] != INDEX_NONE) Place(KeyRooms[c], KeyTypes[c], 7, true);
			}
			// weapons along the way, then ammo for what the player has by then
			auto RoomAt = [&](float F)
			{
				int32 Best = 0;
				for (int32 i = 0; i < Rooms.Num(); ++i)
				{
					if (FMath::Abs(Rooms[i].Depth - F * MaxDepth) < FMath::Abs(Rooms[Best].Depth - F * MaxDepth)) Best = i;
				}
				return Best;
			};
			const int32 ChaingunRoom = RoomAt(0.3f);
			const int32 SuperRoom = Opt.bDoom2 ? RoomAt(0.5f) : INDEX_NONE;
			const int32 RocketRoom = Rooms.Num() >= 8 ? RoomAt(0.65f) : INDEX_NONE;
			const int32 PlasmaRoom = Rooms.Num() >= 14 ? RoomAt(0.85f) : INDEX_NONE;
			Place(ChaingunRoom, 2002, 7);
			if (SuperRoom != INDEX_NONE) Place(SuperRoom, 82, 7);
			if (RocketRoom != INDEX_NONE) Place(RocketRoom, 2003, 7);
			if (PlasmaRoom != INDEX_NONE) Place(PlasmaRoom, 2004, 7);
			Place(RoomAt(0.5f), 2018, 7); // green armour
			if (Rooms.Num() >= 12) Place(RoomAt(0.9f), 2019, 7);

			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				const FRoom& R = Rooms[i];
				const float F = R.Depth / float(MaxDepth);
				if (i != StartRoom)
				{
					const int32 Area = (R.X1 - R.X0) * (R.Y1 - R.Y0);
					const int32 Count = FMath::Max(1, FMath::RoundToInt(Area / 40.f * Opt.MonsterDensity * (0.6f + 0.8f * F)));
					for (int32 m = 0; m < Count; ++m)
					{
						const uint16 Flags = m < FMath::CeilToInt(Count * 0.5f) ? 7 : m < FMath::CeilToInt(Count * 0.8f) ? 6 : 4;
						Report.Monsters += Place(i, Monster(F), Flags) ? 1 : 0;
					}
					if (i == ExitRoom)
					{
						Report.Monsters += Place(i, Opt.bDoom2 ? 69 : 3003, 6) ? 1 : 0;
					}
				}
				// supplies
				const int32 Depth = R.Depth;
				TArray<int32> Ammo = { 2007, 2008 };
				if (Rooms[ChaingunRoom].Depth <= Depth) Ammo.Add(2048);  // box of bullets
				if (RocketRoom != INDEX_NONE && Rooms[RocketRoom].Depth <= Depth) Ammo.Add(2010);
				if (PlasmaRoom != INDEX_NONE && Rooms[PlasmaRoom].Depth <= Depth) Ammo.Add(2047);
				const int32 Supplies = 1 + Rng.RandRange(0, 2);
				for (int32 k = 0; k < Supplies; ++k)
				{
					const float Roll = Rng.FRand();
					const int32 Type = Roll < 0.45f ? Ammo[Rng.RandRange(0, Ammo.Num() - 1)] : Roll < 0.7f ? 2011 : Roll < 0.85f ? 2012 : Roll < 0.95f ? 2014 : 2015;
					Report.Items += Place(i, Type, 7) ? 1 : 0;
				}
				if (Rng.FRand() < 0.25f) Place(i, 2035, 7); // a barrel
			}
		}

		// ---- checks ----

		/** Whether the exit and every key can be reached from the start, walking as Doom does
		 *  (steps up to 24, 56 units of headroom), opening doors whose keys have been picked up. */
		bool Reachable() const
		{
			FIntPoint Start(-1, -1);
			for (const FDoomMapThing& T : Things)
			{
				if (T.Type == 1) Start = FIntPoint(FMath::FloorToInt(T.P.X / TileSize) + W / 2, FMath::FloorToInt(T.P.Y / TileSize) + H / 2);
			}
			if (!In(Start.X, Start.Y)) return false;
			TMap<int32, int32> KeyTiles; // tile -> lock colour
			static const int32 KeyTypes[] = { 0, 5, 6, 13 };
			for (const FDoomMapThing& T : Things)
			{
				for (int32 c = 1; c <= 3; ++c)
				{
					if (T.Type == KeyTypes[c]) KeyTiles.Add(Idx(FMath::FloorToInt(T.P.X / TileSize) + W / 2, FMath::FloorToInt(T.P.Y / TileSize) + H / 2), c);
				}
			}
			auto Floor = [&](int32 Sec) { return Sectors[Sec].Floor; };
			auto Ceil = [&](int32 Sec) { return Sectors[Sec].Kind == EKind::Door ? Sectors[Sec].Floor + 88 : Sectors[Sec].Ceiling; };
			uint32 Keys = 0;
			TArray<uint8> Seen;
			for (int32 Round = 0; Round < 5; ++Round)
			{
				Seen.Init(0, W * H);
				TArray<FIntPoint> Open = { Start };
				Seen[Idx(Start.X, Start.Y)] = 1;
				uint32 NewKeys = Keys;
				while (Open.Num())
				{
					const FIntPoint P = Open.Pop(EAllowShrinking::No);
					if (const int32* C = KeyTiles.Find(Idx(P.X, P.Y))) NewKeys |= 1u << *C;
					const int32 SA = At(P.X, P.Y);
					static const FIntPoint N4[4] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
					for (const FIntPoint& D : N4)
					{
						const FIntPoint Q = P + D;
						const int32 SB = At(Q.X, Q.Y);
						if (SB == INDEX_NONE || Seen[Idx(Q.X, Q.Y)]) continue;
						if (Sectors[SB].Kind == EKind::Door && Sectors[SB].Lock && !(Keys & (1u << Sectors[SB].Lock))) continue;
						if (Floor(SB) - Floor(SA) > MaxStep) continue;
						if (FMath::Min(Ceil(SA), Ceil(SB)) - FMath::Max(Floor(SA), Floor(SB)) < 56) continue;
						Seen[Idx(Q.X, Q.Y)] = 1;
						Open.Add(Q);
					}
				}
				if (NewKeys == Keys) break;
				Keys = NewKeys;
			}
			bool bExit = false;
			for (int32 k = 0; k < Tiles.Num(); ++k)
			{
				bExit |= Seen[k] && Tiles[k] == Alcove;
			}
			for (int32 c = 1; c <= 3; ++c)
			{
				if (KeyRooms[c] != INDEX_NONE && !(Keys & (1u << c))) return false;
			}
			return bExit;
		}

		// ---- the map ----

		void Emit(FDoomMap& Out) const
		{
			Out.Vertices.Reset();
			Out.Lines.Reset();
			Out.Sides.Reset();
			Out.Sectors.Reset();
			Out.Things = Things;
			for (const FGenSector& S : Sectors)
			{
				FDoomSector& D = Out.Sectors.AddDefaulted_GetRef();
				D.Floor = S.Floor;
				D.Ceiling = S.Ceiling;
				D.FloorPic = S.FloorPic;
				D.CeilingPic = S.CeilingPic;
				D.Light = D.OriginalLight = S.Light;
				D.Special = S.Special;
			}
			TMap<int64, int32> VertexAt;
			auto Vertex = [&](int32 GX, int32 GY)
			{
				const int64 Key = (int64)GY * (W + 1) + GX;
				if (const int32* V = VertexAt.Find(Key)) return *V;
				const int32 V = Out.Vertices.Add(FVector2f((GX - W / 2) * TileSize, (GY - H / 2) * TileSize));
				VertexAt.Add(Key, V);
				return V;
			};
			auto IsSky = [&](int32 Sec) { return Sectors[Sec].CeilingPic == FName(TEXT("F_SKY1")); };
			auto MakeSide = [&](int32 Own, int32 Other)
			{
				const FGenSector& S = Sectors[Own];
				FDoomSide Sd;
				Sd.Sector = Own;
				if (Other == INDEX_NONE)
				{
					Sd.Middle = S.Kind == EKind::Door ? S.Track : S.Wall;
					return Sd;
				}
				const FGenSector& O = Sectors[Other];
				if (O.Kind == EKind::Door) Sd.Upper = O.DoorTex;
				else if (O.Ceiling < S.Ceiling && !(IsSky(Own) && IsSky(Other))) Sd.Upper = S.Wall;
				if (O.Floor > S.Floor) Sd.Lower = S.Step;
				return Sd;
			};
			// Front is the right-hand side of V1 -> V2. Doors' faces are fronted by the room or
			// corridor: Doom opens a door from its line's front side only.
			auto AddLine = [&](int32 GX1, int32 GY1, int32 GX2, int32 GY2, int32 Front, int32 Back)
			{
				FDoomLine L;
				L.V1 = Vertex(GX1, GY1);
				L.V2 = Vertex(GX2, GY2);
				const int32 Li = Out.Lines.Num();
				L.Side[0] = Out.Sides.Add(MakeSide(Front, Back));
				if (Back != INDEX_NONE)
				{
					L.Side[1] = Out.Sides.Add(MakeSide(Back, Front));
					L.Flags = DoomLineFlags::TwoSided;
					if (Sectors[Back].Kind == EKind::Door) L.Special = Sectors[Back].DoorSpecial;
				}
				else
				{
					L.Flags = DoomLineFlags::Blocking;
					if (Sectors[Front].Kind == EKind::Door) L.Flags |= DoomLineFlags::DontPegBottom;
				}
				Out.Lines.Add(L);
				return Li;
			};
			auto Order = [&](int32& First, int32& Second)
			{
				// the first is the front unless it is void or a door
				if (First == INDEX_NONE || (Second != INDEX_NONE && Sectors[First].Kind == EKind::Door))
				{
					Swap(First, Second);
					return true;
				}
				return false;
			};
			// edges along X: between the tiles below (y - 1) and above (y)
			for (int32 Y = 0; Y <= H; ++Y)
			{
				for (int32 X = 0; X < W; )
				{
					const int32 Below = At(X, Y - 1), Above = At(X, Y);
					if (Below == Above) { ++X; continue; }
					int32 X1 = X + 1;
					while (X1 < W && At(X1, Y - 1) == Below && At(X1, Y) == Above) ++X1;
					int32 Front = Below, Back = Above;
					const bool bSwapped = Order(Front, Back);
					// below in front: V1 -> V2 runs +X; above in front: -X
					const int32 Li = bSwapped ? AddLine(X1, Y, X, Y, Front, Back) : AddLine(X, Y, X1, Y, Front, Back);
					if (Back == INDEX_NONE && Front == Alcove && bAlcoveAlongX) MakeExitSwitch(Out, Li);
					X = X1;
				}
			}
			// edges along Y: between the tiles left (x - 1) and right (x)
			for (int32 X = 0; X <= W; ++X)
			{
				for (int32 Y = 0; Y < H; )
				{
					const int32 Left = At(X - 1, Y), Right = At(X, Y);
					if (Left == Right) { ++Y; continue; }
					int32 Y1 = Y + 1;
					while (Y1 < H && At(X - 1, Y1) == Left && At(X, Y1) == Right) ++Y1;
					int32 Front = Left, Back = Right;
					const bool bSwapped = Order(Front, Back);
					// left in front: V1 -> V2 runs -Y; right in front: +Y
					const int32 Li = bSwapped ? AddLine(X, Y, X, Y1, Front, Back) : AddLine(X, Y1, X, Y, Front, Back);
					if (Back == INDEX_NONE && Front == Alcove && !bAlcoveAlongX) MakeExitSwitch(Out, Li);
					Y = Y1;
				}
			}
		}

		void MakeExitSwitch(FDoomMap& Out, int32 Li) const
		{
			FDoomLine& L = Out.Lines[Li];
			L.Special = 11; // S1 exit
			Out.Sides[L.Side[0]].Middle = First(ExitSwitches, false);
		}
	};
}

FString DoomMapGen::FReport::ToString() const
{
	return FString::Printf(TEXT("%d rooms, %d corridors (%d doors, %d keys), %d monsters, %d items, %d attempt(s)"),
		Rooms, Links, Doors, Keys, Monsters, Items, Attempts);
}

FString DoomMapGen::NameForSeed(uint32 Seed)
{
	return FString::Printf(TEXT("RND%05X"), Seed & 0xFFFFF);
}

bool DoomMapGen::ParseName(const FString& MapName, uint32& OutSeed)
{
	if (MapName.Len() != 8 || !MapName.StartsWith(TEXT("RND"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	uint32 V = 0;
	for (int32 i = 3; i < 8; ++i)
	{
		if (!FChar::IsHexDigit(MapName[i])) return false;
		V = V * 16 + FParse::HexDigit(MapName[i]);
	}
	OutSeed = V;
	return true;
}

bool DoomMapGen::Generate(const FOptions& Options, const FDoomGraphics* Gfx, FDoomMap& Out, FReport* OutReport)
{
	for (int32 Attempt = 0; Attempt < 16; ++Attempt)
	{
		FGen Gen(Options, Gfx, (int32)(Options.Seed * 7919u + Attempt * 104729u + 17u));
		Gen.Report.Attempts = Attempt + 1;
		Gen.CorridorWall = Gen.Pick(Walls, false);
		Gen.CorridorFloor = Gen.Pick(Floors, true);
		Gen.CorridorCeiling = Gen.Pick(Ceilings, true);
		Gen.StepTex = Gen.First(Steps, false);
		Gen.Track = Gen.First({ TEXT("DOORTRAK") }, false);
		Gen.Layout();
		for (FLink& L : Gen.Links) Gen.MakePath(L);
		Gen.Heights();
		Gen.PaintRooms();
		Gen.PaintCorridors();
		Gen.PaintFeatures();
		if (!Gen.DoorsSound() || !Gen.PaintExit())
		{
			continue;
		}
		Gen.PlaceThings();
		if (!Gen.Reachable())
		{
			continue;
		}
		Out = FDoomMap();
		Out.Name = NameForSeed(Options.Seed);
		Gen.Emit(Out);
		if (!Out.Rebuild())
		{
			continue;
		}
		Gen.Report.Rooms = Gen.Rooms.Num();
		for (const FLink& L : Gen.Links)
		{
			Gen.Report.Links += L.bUsed ? 1 : 0;
			Gen.Report.Doors += (L.bUsed && L.bDoor) ? 1 : 0;
		}
		UE_LOG(LogDoom, Log, TEXT("%s generated: %s; %d sectors, %d lines, %d things, %s"), *Out.Name, *Gen.Report.ToString(),
			Out.Sectors.Num(), Out.Lines.Num(), Out.Things.Num(), *Out.NodesFormat);
		if (OutReport) *OutReport = Gen.Report;
		return true;
	}
	UE_LOG(LogDoom, Error, TEXT("%s: no valid map in 16 attempts"), *NameForSeed(Options.Seed));
	return false;
}

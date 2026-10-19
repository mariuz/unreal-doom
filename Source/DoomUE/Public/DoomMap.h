// A Doom map in Doom's own units and structures. The game logic runs on these
// records; Unreal only presents them (see ADoomLevel).
#pragma once

#include "CoreMinimal.h"

class FDoomWad;

namespace DoomLineFlags
{
	enum : uint16
	{
		Blocking = 1, BlockMonsters = 2, TwoSided = 4, DontPegTop = 8, DontPegBottom = 16,
		Secret = 32, SoundBlock = 64, DontDraw = 128, Mapped = 256
	};
}

struct FDoomLine
{
	int32 V1 = 0, V2 = 0;
	uint16 Flags = 0;
	int16 Special = 0;
	int16 Tag = 0;
	int32 Side[2] = { INDEX_NONE, INDEX_NONE };
	// derived
	FBox2f Bounds;
	int32 FrontSector = INDEX_NONE, BackSector = INDEX_NONE;
	bool bSeen = false; // automap
};

struct FDoomSide
{
	float XOffset = 0, YOffset = 0;
	FName Upper, Lower, Middle;
	int32 Sector = 0;
	int32 Line = INDEX_NONE;
};

struct FDoomSector
{
	float Floor = 0, Ceiling = 0;
	FName FloorPic, CeilingPic;
	int16 Light = 160;
	int16 Special = 0;
	int16 Tag = 0;
	// derived
	TArray<int32> Lines;
	TArray<int32> Subsectors;
	FBox2f Bounds;
	FVector2f Center = FVector2f::ZeroVector;
	// runtime
	int16 OriginalLight = 160;
	bool bSecretFound = false;
	int32 SoundTraversed = 0;   // Doom's soundtraversed (0 none, 1 or 2 lines)
	int32 SoundValid = -1;
	TWeakObjectPtr<class AActor> SoundTarget;
};

struct FDoomSeg
{
	int32 V1 = 0, V2 = 0;
	int32 Line = INDEX_NONE;   // INDEX_NONE: miniseg
	int32 Side = 0;            // 0 front, 1 back
	float Offset = 0;
};

struct FDoomSubsector
{
	int32 FirstSeg = 0, NumSegs = 0;
	int32 Sector = 0;
	TArray<FVector2f> Polygon; // convex, CCW in Doom coordinates
};

struct FDoomNode
{
	FVector2f Origin, Dir;
	int32 Child[2] = { 0, 0 };
	static constexpr int32 SubsectorBit = 0x40000000;
};

struct FDoomMapThing
{
	FVector2f P;
	float Angle = 0;   // degrees
	int32 Type = 0;
	uint16 Flags = 0;  // 1/2/4 skill bits, 8 ambush, 16 multiplayer only
};

struct FLineHit
{
	int32 Line = INDEX_NONE;
	float Frac = 1;     // along the trace 0..1
	int32 Side = 0;     // side of the line the trace started on
};

class DOOMUE_API FDoomMap
{
public:
	bool Load(const FDoomWad& Wad, const FString& MapName);

	FString Name;
	TArray<FVector2f> Vertices;
	TArray<FDoomLine> Lines;
	TArray<FDoomSide> Sides;
	TArray<FDoomSector> Sectors;
	TArray<FDoomSeg> Segs;
	TArray<FDoomSubsector> Subsectors;
	/** Total area of the subsector polygons (E1M1: 6,712,686 square units with vanilla nodes). */
	double SubsectorArea() const;
	TArray<FDoomNode> Nodes;
	TArray<FDoomMapThing> Things;
	FBox2f Bounds;
	/** Which BSP format the map used: vanilla, XNOD, ZGL3, glBSP v5... */
	FString NodesFormat = TEXT("vanilla");
	/** Lines with a special, for walk-over checks. */
	TArray<int32> SpecialLines;

	/** Builds the BSP from the lines and sectors alone, as a node builder does (DoomNodeBuilder.cpp):
	 *  segs (lines cut where a partition crosses them), convex subsectors and nodes, root last.
	 *  For maps made or reshaped by the generator and the editors; replaces any loaded nodes. */
	bool BuildNodes();
	/** After vertices, lines, sides or sectors changed: new nodes, then everything derived from the
	 *  geometry (line and sector links, bounds, subsector polygons). False if the geometry can't make
	 *  a BSP (no lines). */
	bool Rebuild();

	FVector2f V(int32 Index) const { return Vertices[Index]; }

	int32 SubsectorAt(const FVector2f& P) const;
	int32 SectorAt(const FVector2f& P) const { const int32 S = SubsectorAt(P); return S >= 0 ? Subsectors[S].Sector : INDEX_NONE; }

	/** 0 if P is on the front (right) side of the line, 1 on the back. */
	int32 PointOnLineSide(const FVector2f& P, int32 Line) const;

	/** Sector on the other side of Line from Sector, or INDEX_NONE. */
	int32 OtherSector(int32 Line, int32 Sector) const;

	// p_spec-style neighbour queries
	float LowestFloorSurrounding(int32 Sector) const;
	float HighestFloorSurrounding(int32 Sector) const;
	float NextHighestFloor(int32 Sector, float Current) const;
	float LowestCeilingSurrounding(int32 Sector) const;
	float HighestCeilingSurrounding(int32 Sector) const;
	int32 MinSurroundingLight(int32 Sector, int32 Max) const;
	int32 MaxSurroundingLight(int32 Sector, int32 Min) const;

	TArray<int32> SectorsWithTag(int32 Tag) const;

	/** Two-sided opening of a line. False for one-sided lines or closed openings. */
	bool LineOpening(int32 Line, float& OutTop, float& OutBottom) const;

	/** All line crossings of the segment A-B, sorted by distance. */
	void TraceLines(const FVector2f& A, const FVector2f& B, TArray<FLineHit>& OutHits) const;

	/** Whether the 2D segment A->B crosses Line. OutSideA: side A is on. */
	bool SegmentCrossesLine(const FVector2f& A, const FVector2f& B, int32 Line, int32& OutSideA) const;

	/** Distance from P to the line segment. */
	float DistanceToLine(const FVector2f& P, int32 Line) const;

private:
	bool LoadBinary(const FDoomWad& Wad, int32 Marker);
	/** UDMF (TEXTMAP + ZNODES): the doom / zdoom namespaces' vanilla-compatible fields. */
	bool LoadUdmf(const FDoomWad& Wad, int32 Marker);
	bool LoadVanillaNodes(TConstArrayView<uint8> NodesLump, TConstArrayView<uint8> SegsLump, TConstArrayView<uint8> SsLump);
	/** ZDoom extended nodes: XNOD/ZNOD and GL variants XGLN/ZGLN, XGL2/ZGL2, XGL3/ZGL3. */
	bool LoadExtendedNodes(TConstArrayView<uint8> Lump);
	/** glBSP GL nodes (GL_<map>: GL_VERT, GL_SEGS, GL_SSECT, GL_NODES), versions 1-5. */
	bool LoadGlNodes(const FDoomWad& Wad, int32 Marker);
	void AssignSubsectorSectors();
	void BuildSubsectorPolygons();
	void ClipToNode(int32 NodeOrLeaf, TArray<FVector2f> Poly, int32 Depth);
	void Derive();
};

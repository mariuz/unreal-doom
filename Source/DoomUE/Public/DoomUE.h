// DoomUE: DOOM 1 / DOOM 2 on Unreal Engine 5.8.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Parse.h"
#include "Misc/CommandLine.h"

// Log categories by system, so a log can be filtered by area (Output Log filters, -LogCmds="LogDoomAI Verbose"):
DECLARE_LOG_CATEGORY_EXTERN(LogDoom, Log, All);       // boot and the game tables
DECLARE_LOG_CATEGORY_EXTERN(LogDoomGame, Log, All);   // the simulation: world, level flow, saves, menu, actors
DECLARE_LOG_CATEGORY_EXTERN(LogDoomMap, Log, All);    // WADs and maps: loading, nodes, the generator, the checker, PWADs
DECLARE_LOG_CATEGORY_EXTERN(LogDoomRender, Log, All); // level geometry, pictures and textures, materials, tiles
DECLARE_LOG_CATEGORY_EXTERN(LogDoomAudio, Log, All);  // sound effects and music
DECLARE_LOG_CATEGORY_EXTERN(LogDoomNet, Log, All);    // network games: hosting, joining, what clients receive
DECLARE_LOG_CATEGORY_EXTERN(LogDoomAI, Log, All);     // SMART monsters and their paths
DECLARE_LOG_CATEGORY_EXTERN(LogDoomEditor, Log, All); // the map editor and the editor's map actors
DECLARE_LOG_CATEGORY_EXTERN(LogDoomTest, Log, All);   // the test switches' PASS / FAIL lines (-playtest, -nettest...)

class AActor;

/**
 * Coordinates.
 * Doom maps are right-handed (X east, Y north, Z up); Unreal is left-handed
 * (X forward, Y right, Z up). We flip Y and scale: one Doom map unit is
 * DoomScale Unreal units (cm), so the 56-unit marine is 1.68 m tall and
 * Unreal's physics, Lumen and audio attenuation work at sensible sizes.
 */
namespace Doom
{
	constexpr float Scale = 3.0f;
	constexpr int32 TicRate = 35;
	constexpr float TicSeconds = 1.0f / TicRate;

	/** -perfbaseline: the performance work off (idle monsters tick, controllers for everyone, no
	 *  REJECT), to measure it against with -perftest. */
	inline bool PerfBaseline() { static const bool b = FParse::Param(FCommandLine::Get(), TEXT("perfbaseline")); return b; }
	inline FVector ToUE(float X, float Y, float Z) { return FVector(X * Scale, -Y * Scale, Z * Scale); }
	inline FVector ToUE(const FVector2f& P, float Z) { return ToUE(P.X, P.Y, Z); }
	inline FVector2f ToDoom2D(const FVector& V) { return FVector2f(V.X / Scale, -V.Y / Scale); }
	inline float ToDoomZ(float Z) { return Z / Scale; }
	/** Doom angle (degrees CCW from east) to Unreal yaw (degrees CW from +X). */
	inline float AngleToYaw(float DoomDegrees) { return -DoomDegrees; }
	inline float YawToAngle(float Yaw) { return -Yaw; }

	/**
	 * Half height of an actor's root capsule (or sphere radius), whether or not its collision
	 * is enabled. AActor::GetSimpleCollisionHalfHeight falls back to the bounds of every
	 * component when collision is off, which includes the (deliberately oversized) sprite.
	 */
	DOOMUE_API float HalfHeight(const AActor* Actor);
}
